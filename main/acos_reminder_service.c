#include "acos_reminder_service.h"
#include "acos_reminder_config.h"
#include "cfg_service.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "acos_reminder";
static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static reminder_queue_t *s_queue; /* 去重表及消息正文放 PSRAM，队列最多 8 条。 */
static reminder_fragments_t s_fragments; /* 仅 MQTT 事件任务访问；stop 后释放。 */
static char s_topic[280], s_recipient[280];

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void on_mqtt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        /* Topic 含企业 API Key，日志只说明订阅结果，不输出 Topic/载荷。 */
        if (esp_mqtt_client_subscribe(s_client, s_topic, 1) < 0)
            ESP_LOGW(TAG, "Reminder subscribe request failed");
        else ESP_LOGI(TAG, "Connected; subscribing to reminder topic (QoS 1)");
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (event->data && event->data_len > 0 && (unsigned char)event->data[0] == 0x80)
            ESP_LOGW(TAG, "Reminder subscription rejected by broker");
        else ESP_LOGI(TAG, "Reminder subscription acknowledged");
        break;
    case MQTT_EVENT_DISCONNECTED:
        reminder_fragments_reset(&s_fragments);
        ESP_LOGW(TAG, "Disconnected; MQTT will reconnect automatically");
        break;
    case MQTT_EVENT_ERROR:
        ESP_LOGW(TAG, "MQTT transport/broker error; verify network and credentials");
        break;
    case MQTT_EVENT_DATA: {
        if (event->topic_len < 0 || event->data_len < 0 || event->total_data_len < 0 ||
            event->current_data_offset < 0) break;
        if (!reminder_fragments_append(&s_fragments, s_topic, event->topic, event->topic_len,
                event->data, event->data_len, event->total_data_len,
                event->current_data_offset, event->retain)) break;
        reminder_message_t *message = reminder_message_parse(s_fragments.bytes,
                                                            s_fragments.total, s_recipient);
        reminder_fragments_reset(&s_fragments);
        if (!message) { ESP_LOGW(TAG, "Invalid reminder ignored"); break; }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        reminder_enqueue_result_t result = reminder_queue_enqueue(s_queue, message, now_ms(), time(NULL));
        size_t pending = s_queue->count;
        xSemaphoreGive(s_lock);
        static const char *names[] = { "queued", "duplicate", "queue full", "expired" };
        ESP_LOGI(TAG, "Reminder %s; pending=%u", names[result], (unsigned)pending);
        break;
    }
    default: break;
    }
}

esp_err_t acos_reminder_service_init(void)
{
    if (s_queue) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    s_queue = heap_caps_calloc(1, sizeof(*s_queue), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue) { vSemaphoreDelete(s_lock); s_lock = NULL; return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

esp_err_t acos_reminder_service_start(void)
{
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    if (s_client) return ESP_OK; /* 重复 GOT_IP 不创建第二个客户端。 */
    if (!cfg_service_get_bool(CFG_KEY_REMINDER_ENABLED, ACOS_REMINDER_ENABLED)) return ESP_OK;
    char uri[257], key[257], user[257], password[257], client_id[48];
    cfg_service_get_str(CFG_KEY_REMINDER_URI, uri, sizeof(uri), ACOS_REMINDER_URI);
    cfg_service_get_str(CFG_KEY_REMINDER_API_KEY, key, sizeof(key), ACOS_REMINDER_API_KEY);
    if (!key[0] && cfg_service_get_bool(CFG_KEY_REMINDER_USE_TOKEN, ACOS_REMINDER_USE_WS_TOKEN))
        cfg_service_get_str(CFG_KEY_ACOS_TOKEN, key, sizeof(key), "");
    cfg_service_get_str(CFG_KEY_REMINDER_USERNAME, user, sizeof(user), ACOS_REMINDER_USERNAME);
    cfg_service_get_str(CFG_KEY_REMINDER_PASSWORD, password, sizeof(password), ACOS_REMINDER_PASSWORD);
    if (!reminder_api_key_valid(key) || !reminder_broker_uri_valid(uri) ||
        !reminder_credential_valid(user) || !reminder_credential_valid(password)) {
        ESP_LOGW(TAG, "Invalid reminder configuration; check rm_* NVS/defaults");
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t mac[6];
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) return err;
    snprintf(client_id, sizeof(client_id), "ymos-reminder-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_topic, sizeof(s_topic), "acos/%s/remind", key);
    snprintf(s_recipient, sizeof(s_recipient), "device:%s", key);
    esp_mqtt_client_config_t config = {
        .broker.address.uri = uri,
        .broker.verification.crt_bundle_attach = strncmp(uri, "mqtts://", 8) == 0
                                                   ? esp_crt_bundle_attach : NULL,
        .credentials.client_id = client_id,
        .credentials.username = user[0] ? user : NULL,
        .credentials.authentication.password = password[0] ? password : NULL,
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .session.keepalive = 60,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 5000,
        .task.priority = 2,
        .task.stack_size = 8192,
        .buffer.size = 1024,
        .buffer.out_size = 512,
        .outbox.limit = 2048,
    };
    /* esp-mqtt init 深拷贝配置字符串，局部凭据不跨任务借用。 */
    s_client = esp_mqtt_client_init(&config);
    if (!s_client) return ESP_ERR_NO_MEM;
    err = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_mqtt, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) { esp_mqtt_client_destroy(s_client); s_client = NULL; }
    return err;
}

esp_err_t acos_reminder_service_stop(void)
{
    if (!s_client) return ESP_OK;
    esp_err_t err = esp_mqtt_client_stop(s_client);
    if (err != ESP_OK) return err; /* 未停稳不得释放回调仍可能使用的分片。 */
    err = esp_mqtt_client_destroy(s_client);
    if (err != ESP_OK) return err;
    s_client = NULL;
    reminder_fragments_reset(&s_fragments);
    return ESP_OK;
}

const reminder_message_t *acos_reminder_service_take(void)
{
    if (!s_queue) return NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const reminder_message_t *message = reminder_queue_take(s_queue, now_ms(), time(NULL));
    xSemaphoreGive(s_lock);
    return message;
}

void acos_reminder_service_finish(reminder_outcome_t outcome)
{
    if (!s_queue) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    reminder_queue_finish(s_queue, outcome, now_ms());
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Reminder %s", outcome == REMINDER_COMPLETED ? "completed" :
             outcome == REMINDER_CANCELLED ? "cancelled" : "failed");
}
