/** SoftAP 网页配网。app_main 串行控制生命周期；HTTP/DNS 只处理请求和发布事件。 */
#include "provisioning_service.h"
#include "cfg_service.h"
#include "wifi_service.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "provisioning";
static httpd_handle_t s_http;
static atomic_bool s_active, s_scan_ready, s_scan_busy;
static atomic_uint s_generation;
static bool s_dns_running, s_scan_running;
static bool s_submitted; /* HTTP 任务拥有；主任务先停 HTTP 再确认/回滚。 */
static SemaphoreHandle_t s_dns_done, s_scan_done;
static wifi_ap_record_t s_scan_records[20];
static uint16_t s_scan_count;
static esp_timer_handle_t s_timeout_timer;
static int64_t s_deadline_us; /* 主任务写/读，用于拒绝上一轮残留的超时事件。 */
static char s_old_ssid[33], s_old_pass[65];
static bool s_have_backup;

static void provision_timeout(void *arg)
{
    (void)arg;
    event_bus_publish(EV_PROVISIONING_TIMEOUT, NULL, 0);
}

static void dns_task(void *arg)
{
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd >= 0) {
        struct timeval timeout = {.tv_usec = 200000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        struct sockaddr_in local = {
            .sin_family = AF_INET, .sin_port = htons(53),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0) {
            uint8_t buffer[512];
            while (atomic_load(&s_active)) {
                struct sockaddr_in peer;
                socklen_t peer_len = sizeof(peer);
                int n = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&peer, &peer_len);
                if (n < 12) continue;
                int q = 12;
                while (q < n && buffer[q] && buffer[q] <= 63) q += buffer[q] + 1;
                if (q >= n || buffer[q] != 0 || q + 5 > n || q + 21 > sizeof(buffer)) continue;
                q += 5;
                /* 仅返回 A/IN 查询；其他类型返回空响应，不能把 IPv4 数据冒充 AAAA。 */
                bool ipv4 = buffer[q - 4] == 0 && buffer[q - 3] == 1 &&
                            buffer[q - 2] == 0 && buffer[q - 1] == 1;
                buffer[2] = 0x81; buffer[3] = 0x80;
                buffer[4] = 0; buffer[5] = 1;
                buffer[6] = 0; buffer[7] = ipv4 ? 1 : 0;
                buffer[8] = buffer[9] = buffer[10] = buffer[11] = 0;
                static const uint8_t answer[] = {
                    0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4, 192, 168, 4, 1,
                };
                if (ipv4) { memcpy(buffer + q, answer, sizeof(answer)); q += sizeof(answer); }
                sendto(fd, buffer, q, 0, (struct sockaddr *)&peer, peer_len);
            }
        } else ESP_LOGE(TAG, "Captive DNS bind failed");
        close(fd);
    } else ESP_LOGE(TAG, "Captive DNS socket failed");
    /* 自行关闭 socket 后再确认退出，反复进出配网不强杀持有 socket 的任务。 */
    xSemaphoreGive(s_dns_done);
    vTaskDelete(NULL);
}

static esp_err_t captive_get(httpd_req_t *request)
{
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
    return httpd_resp_sendstr(request, "Redirecting to YMOS Wi-Fi setup");
}

static const char s_page[] = "<!doctype html><meta charset=utf-8><title>YMOS 配网</title><h2>YMOS Wi-Fi 配网</h2><button onclick=scan()>扫描 Wi-Fi</button><select id=ssid></select><br><input id=pass type=password placeholder=Wi-Fi密码><button onclick=save()>保存并连接</button><pre id=msg></pre><script>async function scan(){msg.textContent='扫描中...';await fetch('/api/scan');for(let i=0;i<20;i++){let a=await(await fetch('/api/scan/result')).json();if(a.ready){ssid.innerHTML=a.networks.map(x=>`<option>${x.ssid}</option>`).join('');msg.textContent='请选择 Wi-Fi 并输入密码';return}await new Promise(r=>setTimeout(r,500))}msg.textContent='扫描超时'}async function save(){let r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid.value,password:pass.value})});msg.textContent=await r.text()}</script>";

static esp_err_t page_get(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, s_page, HTTPD_RESP_USE_STRLEN);
}

static void scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t config = {0};
    s_scan_count = 0;
    if (esp_wifi_scan_start(&config, true) == ESP_OK) {
        s_scan_count = 20;
        if (esp_wifi_scan_get_ap_records(&s_scan_count, s_scan_records) != ESP_OK) s_scan_count = 0;
    }
    atomic_store(&s_scan_ready, true);  /* 发布完成后 HTTP 才可读取结果数组。 */
    atomic_store(&s_scan_busy, false);
    xSemaphoreGive(s_scan_done);
    vTaskDelete(NULL);
}

static esp_err_t scan_get(httpd_req_t *request)
{
    if (!atomic_load(&s_scan_busy)) {
        if (s_scan_running) xSemaphoreTake(s_scan_done, portMAX_DELAY);
        s_scan_running = false;
        atomic_store(&s_scan_ready, false);
        atomic_store(&s_scan_busy, true);
        if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
            s_scan_count = 0;
            atomic_store(&s_scan_busy, false);
            atomic_store(&s_scan_ready, true);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "scan task failed");
        }
        s_scan_running = true;
    }
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"ready\":false,\"networks\":[]}");
}

static esp_err_t scan_result_get(httpd_req_t *request)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    bool ready = atomic_load(&s_scan_ready);
    cJSON_AddBoolToObject(root, "ready", ready);
    cJSON *networks = cJSON_AddArrayToObject(root, "networks");
    for (int i = 0; ready && networks && i < s_scan_count; ++i) {
        if (!s_scan_records[i].ssid[0]) continue;
        cJSON *network = cJSON_CreateObject();
        if (!network) break;
        /* JSON 库处理 SSID 中的引号/反斜杠，避免配网页面解析失败。 */
        cJSON_AddStringToObject(network, "ssid", (const char *)s_scan_records[i].ssid);
        cJSON_AddNumberToObject(network, "rssi", s_scan_records[i].rssi);
        cJSON_AddNumberToObject(network, "auth", s_scan_records[i].authmode);
        cJSON_AddItemToArray(networks, network);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(request, "application/json");
    esp_err_t err = httpd_resp_sendstr(request, json);
    cJSON_free(json);
    return err;
}

static esp_err_t info_get(httpd_req_t *request)
{
    char chip[20] = {0}, device[64] = {0}, client[64] = {0}, json[256];
    cfg_service_get_identity(chip, sizeof(chip), device, sizeof(device), client, sizeof(client));
    snprintf(json, sizeof(json), "{\"device_id\":\"%s\",\"chip_id\":\"%s\",\"provisioned\":%s}",
             device, chip, cfg_service_is_paired() ? "true" : "false");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, json);
}

static esp_err_t reset_post(httpd_req_t *request)
{
    cfg_service_erase(CFG_KEY_WIFI_SSID);
    cfg_service_erase(CFG_KEY_WIFI_PASSWORD);
    cfg_service_erase(CFG_KEY_PAIRED);
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

static esp_err_t wifi_post(httpd_req_t *request)
{
    if (s_submitted) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "already submitted");
    if (request->content_len <= 0 || request->content_len >= 256) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid body");
    }
    char body[256] = {0};
    int received = 0;
    while (received < request->content_len) {
        int n = httpd_req_recv(request, body + received, request->content_len - received);
        if (n <= 0) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "read failed");
        received += n;
    }
    cJSON *root = cJSON_ParseWithLength(body, received);
    cJSON *ssid = root ? cJSON_GetObjectItemCaseSensitive(root, "ssid") : NULL;
    cJSON *password = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password) || !ssid->valuestring[0] ||
        strlen(ssid->valuestring) > 32 || strlen(password->valuestring) > 64) {
        cJSON_Delete(root);
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "invalid ssid/password");
    }
    cfg_get_wifi_credential(s_old_ssid, sizeof(s_old_ssid), s_old_pass, sizeof(s_old_pass));
    s_have_backup = true;
    esp_err_t err = cfg_service_set_str(CFG_KEY_WIFI_SSID, ssid->valuestring);
    if (err == ESP_OK) err = cfg_service_set_str(CFG_KEY_WIFI_PASSWORD, password->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        provisioning_service_rollback();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    if (event_bus_publish(EV_PROVISIONING_DONE,
                          (void *)(uintptr_t)atomic_load(&s_generation), 0) != pdPASS) {
        provisioning_service_rollback();
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "busy; please retry");
    }
    s_submitted = true;
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

esp_err_t provisioning_service_init(event_bus_t bus)
{
    (void)bus;
    s_dns_done = xSemaphoreCreateBinary();
    s_scan_done = xSemaphoreCreateBinary();
    if (!s_dns_done || !s_scan_done) return ESP_ERR_NO_MEM;
    esp_timer_create_args_t args = {.callback = provision_timeout, .name = "prov_timeout"};
    return esp_timer_create(&args, &s_timeout_timer);
}

esp_err_t provisioning_service_start(void)
{
    if (atomic_load(&s_active)) return ESP_OK;
    esp_err_t err = wifi_service_disconnect();
    if (err != ESP_OK) return err;
    wifi_config_t ap = {.ap = {.channel = 1, .max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "YMOS-%02X%02X",
             (unsigned char)esp_random(), (unsigned char)esp_random());
    strcpy((char *)ap.ap.password, "ymos1234");
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    err = esp_wifi_set_mode(WIFI_MODE_APSTA);  /* STA 仅用于扫描，不自动连接旧路由器。 */
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) { wifi_service_disconnect(); return err; }
    atomic_fetch_add(&s_generation, 1);
    s_submitted = false;
    s_scan_count = 0;
    atomic_store(&s_scan_ready, true);
    atomic_store(&s_active, true);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    err = httpd_start(&s_http, &config);
    if (err != ESP_OK) goto fail;
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/info", .method = HTTP_GET, .handler = info_get},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get},
        {.uri = "/api/scan/result", .method = HTTP_GET, .handler = scan_result_get},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post},
        {.uri = "/api/reset", .method = HTTP_POST, .handler = reset_post},
        {.uri = "/*", .method = HTTP_GET, .handler = captive_get},
    };
    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
        err = httpd_register_uri_handler(s_http, &handlers[i]);
        if (err != ESP_OK) goto fail;
    }
    xSemaphoreTake(s_dns_done, 0);
    if (xTaskCreate(dns_task, "captive_dns", 3072, NULL, 4, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    s_dns_running = true;
    s_deadline_us = esp_timer_get_time() + 600000000;
    err = esp_timer_start_once(s_timeout_timer, 600000000);
    if (err != ESP_OK) goto fail;
    ESP_LOGI(TAG, "SoftAP provisioning: SSID=%s, open http://192.168.4.1/", ap.ap.ssid);
    return ESP_OK;
fail:
    provisioning_service_stop();
    return err;
}

esp_err_t provisioning_service_stop(void)
{
    s_deadline_us = 0;
    if (s_timeout_timer) esp_timer_stop(s_timeout_timer);
    atomic_store(&s_active, false);
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
    if (s_scan_running) {
        if (atomic_load(&s_scan_busy)) esp_wifi_scan_stop();
        xSemaphoreTake(s_scan_done, portMAX_DELAY);
        s_scan_running = false;
    }
    if (s_dns_running) {
        xSemaphoreTake(s_dns_done, portMAX_DELAY);
        s_dns_running = false;
    }
    esp_err_t err = wifi_service_disconnect();
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    return err;
}

bool provisioning_service_is_active(void)
{
    return atomic_load(&s_active);
}

uint32_t provisioning_service_generation(void)
{
    return atomic_load(&s_generation);
}

bool provisioning_service_timed_out(void)
{
    return s_deadline_us && esp_timer_get_time() >= s_deadline_us;
}

void provisioning_service_confirm(void)
{
    /* 只有拿到 IP 才提交配对状态，防止日后失败时回滚到早已过期的旧配置。 */
    if (s_have_backup || !cfg_service_is_paired()) {
        esp_err_t err = cfg_service_mark_paired();
        if (err != ESP_OK) ESP_LOGW(TAG, "Save paired state failed: %s", esp_err_to_name(err));
    }
    s_have_backup = false;
}

void provisioning_service_rollback(void)
{
    if (!s_have_backup) return;
    esp_err_t err = cfg_service_set_str(CFG_KEY_WIFI_SSID, s_old_ssid);
    if (err == ESP_OK) err = cfg_service_set_str(CFG_KEY_WIFI_PASSWORD, s_old_pass);
    if (err == ESP_OK) s_have_backup = false;
    else ESP_LOGE(TAG, "Credential rollback failed: %s", esp_err_to_name(err));
}
