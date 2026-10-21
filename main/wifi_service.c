/**
 * wifi_service.c — Wi-Fi 连接管理实现
 */

#include "wifi_service.h"
#include "event_bus.h"
#include "led_service.h"

#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"

static const char *TAG = "wifi_service";

/* ── 事件位 ───────────────────────────────────────────────────────────────── */

#define WIFI_CONNECTED_BIT   (1 << 0)
#define WIFI_GOT_IP_BIT     (1 << 1)
#define WIFI_FAIL_BIT        (1 << 2)

/* ── 内部状态 ─────────────────────────────────────────────────────────────── */

static EventGroupHandle_t s_wifi_event_group = NULL;
static wifi_state_t s_wifi_state = WIFI_STATE_IDLE;
static SemaphoreHandle_t s_mutex = NULL;
/* 主动停止/AP 配网期间禁止 STA_START 和断线事件自动重连。 */
static atomic_bool s_connect_enabled;
static esp_timer_handle_t s_connect_timer = NULL;
static int64_t s_connect_deadline_us; /* 与 start/stop/事件处理共用递归互斥锁。 */
#define WIFI_CONNECT_TIMEOUT_MS 60000
static wifi_state_t get_state(void);
static void set_state(wifi_state_t st);
static void lock(void);
static void unlock(void);

static void wifi_connect_timeout(void *arg)
{
    (void)arg;
    lock();
    /* 已排队的旧回调不能中止刚开始的新一轮连接。 */
    if (s_connect_deadline_us && esp_timer_get_time() >= s_connect_deadline_us &&
        atomic_exchange(&s_connect_enabled, false)) {
        s_connect_deadline_us = 0;
        ESP_LOGW(TAG, "Wi-Fi connection timeout after %d ms", WIFI_CONNECT_TIMEOUT_MS);
        esp_wifi_disconnect();
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        set_state(WIFI_STATE_FAIL);
    }
    unlock();
}

/* ── 工具 ─────────────────────────────────────────────────────────────────── */

static void lock(void)   { xSemaphoreTakeRecursive(s_mutex, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGiveRecursive(s_mutex); }

static void set_state(wifi_state_t st)
{
    lock();
    wifi_state_t prev = s_wifi_state;
    s_wifi_state = st;
    unlock();

    if (prev != st) {
        ESP_LOGD(TAG, "wifi_state %d → %d", prev, st);
        if (st == WIFI_STATE_GOT_IP) event_bus_publish(EV_WIFI_GOT_IP, NULL, 0);
        else if (st == WIFI_STATE_FAIL) event_bus_publish(EV_WIFI_CONNECT_FAILED, NULL, 0);
        else if (st == WIFI_STATE_DISCONNECTED) event_bus_publish(EV_WIFI_DISCONNECTED, NULL, 0);
    }
}

static wifi_state_t get_state(void)
{
    lock();
    wifi_state_t st = s_wifi_state;
    unlock();
    return st;
}

/* ── 事件处理器 ───────────────────────────────────────────────────────────── */

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    lock();
    if (!atomic_load(&s_connect_enabled)) { unlock(); return; }
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, initiating connection");
                esp_err_t connect_err = esp_wifi_connect();
                if (connect_err != ESP_OK && connect_err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(connect_err));
                }
                break;

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "Wi-Fi connected (SSID matched)");
                set_state(WIFI_STATE_CONNECTED);
                led_set_wifi_err();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *ev =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "Disconnected: reason=%d; retrying", ev->reason);
                xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
                set_state(WIFI_STATE_DISCONNECTED);
                if (!esp_timer_is_active(s_connect_timer)) {
                    s_connect_deadline_us = esp_timer_get_time() + WIFI_CONNECT_TIMEOUT_MS * 1000ULL;
                    esp_timer_start_once(s_connect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
                }
                esp_err_t retry_err = esp_wifi_connect();
                if (retry_err != ESP_OK && retry_err != ESP_ERR_WIFI_CONN) {
                    ESP_LOGW(TAG, "Reconnect failed: %s", esp_err_to_name(retry_err));
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
                ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
                set_state(WIFI_STATE_GOT_IP);
                s_connect_deadline_us = 0;
                if (s_connect_timer) esp_timer_stop(s_connect_timer);
                led_set_wifi_ok();
                xEventGroupSetBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
                break;
            }
            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "Lost IP");
                xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT);
                set_state(WIFI_STATE_DISCONNECTED);
                if (!esp_timer_is_active(s_connect_timer)) {
                    s_connect_deadline_us = esp_timer_get_time() + WIFI_CONNECT_TIMEOUT_MS * 1000ULL;
                    esp_timer_start_once(s_connect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
                }
                break;
            default:
                break;
        }
    }
    unlock();
}

/* ── 公开 API ─────────────────────────────────────────────────────────────── */

esp_err_t wifi_service_init(void)
{
    s_mutex = xSemaphoreCreateRecursiveMutex();
    if (!s_mutex) return ESP_FAIL;

    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) return ESP_FAIL;
    const esp_timer_create_args_t timer_args = {
        .callback = wifi_connect_timeout,
        .name = "wifi_connect_timeout",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_connect_timer));

    /* 初始化 TCP/IP / netif */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* 初始化 Wi-Fi 驱动 */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 注册事件处理 */
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                               &wifi_event_handler, NULL));

    /* STA 模式 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    set_state(WIFI_STATE_IDLE);
    ESP_LOGI(TAG, "wifi_service init ok");
    return ESP_OK;
}

esp_err_t wifi_service_connect(const char *ssid, const char *password,
                                uint32_t timeout_ms)
{
    esp_err_t err = wifi_service_start(ssid, password);
    if (err != ESP_OK) return err;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
        WIFI_GOT_IP_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (bits & WIFI_GOT_IP_BIT) return ESP_OK;
    wifi_service_disconnect();
    return bits & WIFI_FAIL_BIT ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_start(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0] || strlen(ssid) > 32 ||
        (password && strlen(password) > 64)) return ESP_ERR_INVALID_ARG;
    lock();
    esp_err_t err = wifi_service_disconnect();
    if (err != ESP_OK) { unlock(); return err; }
    wifi_config_t config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_OPEN,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };
    /* ESP-IDF 的这两个数组允许恰好 32/64 字节，不强行截掉最后一字节。 */
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    if (password && password[0]) {
        memcpy(config.sta.password, password, strlen(password));
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) { unlock(); return err; }
    xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
    set_state(WIFI_STATE_CONNECTING);
    atomic_store(&s_connect_enabled, true);
    s_connect_deadline_us = esp_timer_get_time() + WIFI_CONNECT_TIMEOUT_MS * 1000ULL;
    err = esp_timer_start_once(s_connect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err != ESP_OK) {
        wifi_service_disconnect();
        set_state(WIFI_STATE_FAIL);
        unlock();
        return err;
    }
    ESP_LOGI(TAG, "Wi-Fi connection started (timeout=%d ms)", WIFI_CONNECT_TIMEOUT_MS);
    unlock();
    return ESP_OK;
}

esp_err_t wifi_service_disconnect(void)
{
    lock();
    s_connect_deadline_us = 0;
    atomic_store(&s_connect_enabled, false);
    if (s_connect_timer) esp_timer_stop(s_connect_timer);
    /* 必须 stop 驱动，下一次 start 才可靠收到 STA_START；AP 配网由其服务接管。 */
    esp_err_t err = esp_wifi_stop();
    if (err == ESP_ERR_WIFI_NOT_STARTED) err = ESP_OK;
    if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_GOT_IP_BIT | WIFI_FAIL_BIT);
    set_state(WIFI_STATE_IDLE);
    unlock();
    return err;
}

wifi_state_t wifi_service_get_state(void)
{
    return get_state();
}

bool wifi_service_get_ip_str(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 16) return false;

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) {
        return false;
    }

    snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    return true;
}

bool wifi_service_get_mac_str(char *buf, size_t buf_size, esp_mac_type_t type)
{
    if (!buf || buf_size < 18) return false;

    uint8_t mac[6];
    if (esp_read_mac(mac, type) != ESP_OK) {
        return false;
    }

    snprintf(buf, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return true;
}

int8_t wifi_service_get_rssi(void)
{
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.rssi;
}

esp_err_t wifi_service_deinit(void)
{
    wifi_service_disconnect();
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, &wifi_event_handler);
    if (s_connect_timer) {
        esp_timer_stop(s_connect_timer);
        esp_timer_delete(s_connect_timer);
        s_connect_timer = NULL;
    }
    atomic_store(&s_connect_enabled, false);
    set_state(WIFI_STATE_IDLE);
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }

    return ESP_OK;
}
