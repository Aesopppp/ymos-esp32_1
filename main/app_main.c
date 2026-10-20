/**
 * @file app_main.c
 * @brief Korvo Agent 入口 — FreeRTOS 任务编排与事件分发中枢
 *
 * ══════════════════════════════════════════════════════════════════════════════
 * 系统架构
 * ══════════════════════════════════════════════════════════════════════════════
 * P0 收音链路：MIC(ES7210) → I²S DMA → AFE(WakeNet VAD) → RingBuffer
 *              → audio_in(VAD切句) → Opus编码 → WS JSON帧 → ACOS云端
 *
 * P1放音链路：ACOS WS(Opus) → Opus解码 → ES8311 DAC → NS4150功放 → 扬声器
 *              支持 Barge-In 打断
 *
 * 依赖：ESP-IDF v6.1 + esp-sr
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_flash.h"
#include "esp_psram.h"
#include "esp_event.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "board_config.h"
#include "event_bus.h"
#include "app_state.h"
#include "wifi_service.h"
#include "cfg_service.h"
#include "log_service.h"
#include "led_service.h"
#include "audio_out.h"
#include "ws_service.h"
#include "proto_service.h"
#include "audio_in.h"
#include "wake_word.h"
#include "mqtt_service.h"
#include "provisioning_service.h"
#include "button_service.h"
#include "button_policy.h"
#include "cJSON.h"

static const char *TAG = "app_main";

/* ── 内部任务 & 队列 ────────────────────────────────────────── */

static event_bus_t s_bus;
static led_service_t s_led;
static int s_saved_volume;
static int64_t s_volume_save_at_ms;


#define MAIN_TASK_PRIORITY   5
#define MAIN_TASK_STACK     8192

/* ── 系统初始化 ─────────────────────────────────────────────── */

static esp_err_t system_init(void)
{
    ESP_LOGI(TAG, "=== Korvo Agent starting (ESP-IDF v6.1) ===");

#if CONFIG_SPIRAM
    if (esp_psram_get_size() > 0) {
        ESP_LOGI(TAG, "PSRAM size: %zu MB", esp_psram_get_size() / 1024 / 1024);
    }
#endif

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(cfg_service_init());
    ESP_ERROR_CHECK(app_state_init(s_bus));
    /* wifi_service_init owns netif and default event loop creation. */
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_LOGI(TAG, "Wi-Fi service ready");

    s_led = led_service_init();
    if (s_led == NULL) {
        ESP_LOGE(TAG, "LED service init failed");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(audio_out_init(ES8311_I2C_ADDR));
    int volume = cfg_service_get_int(CFG_KEY_VOLUME, AUDIO_OUT_DEFAULT_VOLUME);
    if (volume < 0 || volume > 100) volume = AUDIO_OUT_DEFAULT_VOLUME;
    ESP_ERROR_CHECK(audio_out_set_volume(volume));
    s_saved_volume = volume;
    ESP_LOGI(TAG, "Audio output ready (volume=%d)", volume);


    ESP_ERROR_CHECK(ws_service_init());
    ESP_ERROR_CHECK(mqtt_service_init(s_bus));
    ESP_ERROR_CHECK(provisioning_service_init(s_bus));
    audio_in_config_t input_config = audio_in_default_config();
    input_config.bus = s_bus;
    if (!audio_in_create(&input_config)) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(wake_word_init());
    ESP_LOGI(TAG, "WebSocket service ready");
    esp_err_t button_err = button_service_init();
    if (button_err != ESP_OK) {
        ESP_LOGW(TAG, "Buttons unavailable: %s", esp_err_to_name(button_err));
    }

    return ESP_OK;
}

/* ── 事件分发 ──────────────────────────────────────────────── */

/* Only this task changes session state or starts/stops the WS client. */
static bool conversation_active;
/* 主任务拥有。结束对话/进入配网先清除此意图，迟到的错误不得重新拉起会话。 */
static bool session_requested;
static int64_t retry_at_ms, auth_deadline_ms, cancel_deadline_ms, response_deadline_ms;
static int64_t resume_deadline_ms;
static int64_t cancel_probe_ms;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void end_conversation(const char *reason)
{
    session_requested = conversation_active = false;
    retry_at_ms = auth_deadline_ms = cancel_deadline_ms = response_deadline_ms = 0;
    resume_deadline_ms = cancel_probe_ms = 0;
    /* 先停上传/播放，再启用 WakeNet，避免结束时把自己的尾音当成新唤醒。 */
    app_state_pause_silence_timer();
    esp_err_t input_err = audio_in_stop(audio_in_get_handle());
    if (input_err != ESP_OK) ESP_LOGW(TAG, "Audio stop failed: %s", esp_err_to_name(input_err));
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) ESP_LOGW(TAG, "WS stop failed: %s", esp_err_to_name(err));
    app_state_back_to_wait_wake(false);
    wake_word_reset_barge_in();
    wake_word_clear_audio_buffer();
    if (wifi_service_get_state() == WIFI_STATE_GOT_IP) led_set_waiting_wake();
    else led_set_wifi_err();
    ESP_LOGI(TAG, "Conversation ended: %s; waiting for wake word or REC", reason);
}

static void recover_session(const char *reason)
{
    if (!session_requested || provisioning_service_is_active()) return;
    ESP_LOGW(TAG, "Session recovery: %s", reason);
    if (conversation_active && !resume_deadline_ms) resume_deadline_ms = now_ms() + 60000;
    app_state_back_to_wait_wake(false);
    wake_word_reset_barge_in();
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) ESP_LOGW(TAG, "WS stop pending: %s", esp_err_to_name(err));
    wake_word_clear_audio_buffer();
    auth_deadline_ms = cancel_deadline_ms = response_deadline_ms = 0;
    cancel_probe_ms = 0;
    retry_at_ms = now_ms() + 5000;
    ESP_LOGW(TAG, "Reconnect scheduled in 5000 ms; resume_conversation=%d (no voice required)",
             conversation_active);
    led_set_ws_err();
}

static void resume_listening(bool keep_buffered_speech, const char *reason)
{
    if (!keep_buffered_speech) wake_word_clear_audio_buffer();
    cancel_deadline_ms = response_deadline_ms = 0;
    cancel_probe_ms = 0;
    if (app_state_start_listening() != ESP_OK) {
        recover_session("could not start audio upload");
        return;
    }
    wake_word_reset_barge_in();

    /* 唤醒、按键、重连、取消和正常回复统一保证上传时使用 LITE 模式。 */
    if (wake_word_get_mode() != WAKE_WORD_MODE_LITE) {
        if (wake_word_set_mode(WAKE_WORD_MODE_LITE) == ESP_OK) {
            ESP_LOGI(TAG, "AFE mode -> LITE (WakeNet disabled for audio)");
        }
    }
    resume_deadline_ms = 0;
    led_set_listening();
    ESP_LOGI(TAG, "Conversation listening; source=%s; ready for the next question", reason);
}

/* 唤醒词和 REC 共用入口：待机不上传，认证成功后才开始监听。 */
static void start_conversation(const char *reason)
{
    if (provisioning_service_is_active()) {
        wake_word_reset();
        ESP_LOGI(TAG, "Talk ignored during Wi-Fi setup");
        return;
    }
    if (wifi_service_get_state() != WIFI_STATE_GOT_IP) {
        wake_word_reset();
        led_set_wifi_err();
        ESP_LOGW(TAG, "Talk ignored: Wi-Fi not ready");
        return;
    }
    if (session_requested) {
        if (app_state_get() == APP_STATE_LISTENING) app_state_reset_silence_timer();
        return; /* 连接中/取消中重复按键不重复发送请求。 */
    }
    session_requested = true;
    wake_word_set_mode(WAKE_WORD_MODE_LITE);
    if (ws_service_get_state() == WS_STATE_CONNECTED) {
        conversation_active = true;
        resume_listening(false, reason);
    } else {
        app_state_set(APP_STATE_WS_CONNECTING_WAKE);
        retry_at_ms = now_ms();
        led_set_connecting();
        ESP_LOGI(TAG, "Conversation requested: %s; opening WebSocket", reason);
    }
}

/* 手动/语音打断共用取消边界；只在主任务操作 WS，保留已有预录音补传逻辑。 */
static void interrupt_response(bool manual)
{
    if (!conversation_active || app_state_get() != APP_STATE_PLAYING ||
        ws_service_get_state() != WS_STATE_CONNECTED) return;
    if (manual) wake_word_protect_preroll();
    esp_err_t err = ws_service_cancel_response();
    if (err == ESP_ERR_NOT_FOUND) {
        /* 回答恰好结束，队列内的正常完成事件负责恢复监听。 */
        wake_word_reset_barge_in();
        return;
    }
    if (err != ESP_OK) { recover_session("response.cancel send failed"); return; }
    app_state_set(APP_STATE_CANCELLING);
    app_state_pause_silence_timer();
    response_deadline_ms = 0;
    cancel_probe_ms = now_ms() + 3000;
    cancel_deadline_ms = now_ms() + 8000;
    ESP_LOGI(TAG, "Playback stopped (%s); waiting for cancelled reply boundary",
             manual ? "REC" : "voice");
}

static esp_err_t start_saved_wifi(void)
{
    char ssid[33], password[65];
    cfg_get_wifi_credential(ssid, sizeof(ssid), password, sizeof(password));
    if (!ssid[0]) return ESP_ERR_NOT_FOUND;
    esp_err_t err = wifi_service_start(ssid, password);
    if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi start failed: %s", esp_err_to_name(err));
    return err;
}

static void enter_provisioning(const char *reason)
{
    if (provisioning_service_is_active()) return;
    end_conversation(reason);
    (void)mqtt_service_stop();
    esp_err_t err = provisioning_service_start();
    if (err == ESP_OK) {
        app_state_set(APP_STATE_PROVISIONING);
        led_set_connecting();
        ESP_LOGI(TAG, "Wi-Fi setup active; hold SET for 3s to leave");
    } else {
        ESP_LOGE(TAG, "Wi-Fi setup failed: %s", esp_err_to_name(err));
        (void)start_saved_wifi();
    }
}

static void leave_provisioning(const char *reason)
{
    if (!provisioning_service_is_active()) return;
    esp_err_t err = provisioning_service_stop();
    app_state_back_to_wait_wake(false);
    led_set_wifi_err();
    ESP_LOGI(TAG, "Wi-Fi setup closed: %s; reconnecting saved network", reason);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi setup stop failed: %s", esp_err_to_name(err));
        return;
    }
    if (start_saved_wifi() == ESP_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved network; hold SET for 3s to configure Wi-Fi");
    }
}

static void change_volume(bool increase)
{
    int current = audio_out_get_volume();
    int next = button_volume_next(current, increase);
    if (next == current) return;
    esp_err_t err = audio_out_set_volume(next);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Volume change failed: %s", esp_err_to_name(err));
        return;
    }
    /* 连按只修改 RAM/Codec，停止调节 1.5 秒后合并写一次 NVS。 */
    s_volume_save_at_ms = now_ms() + 1500;
    ESP_LOGI(TAG, "Volume: %d -> %d", current, next);
}

static void save_volume_if_due(int64_t now)
{
    if (!s_volume_save_at_ms || now < s_volume_save_at_ms) return;
    int volume = audio_out_get_volume();
    if (volume == s_saved_volume) { s_volume_save_at_ms = 0; return; }
    esp_err_t err = cfg_service_set_int(CFG_KEY_VOLUME, volume);
    if (err == ESP_OK) {
        s_saved_volume = volume;
        s_volume_save_at_ms = 0;
        ESP_LOGI(TAG, "Volume saved: %d", volume);
    } else {
        s_volume_save_at_ms = now + 5000;
        ESP_LOGW(TAG, "Volume save failed; retry in 5s: %s", esp_err_to_name(err));
    }
}

static bool tagged_ws_event(event_type_t id)
{
    return id == EV_WS_CONNECTED || id == EV_WS_AUTH_OK || id == EV_WS_ERROR ||
           id == EV_WS_DISCONNECTED || id == EV_WS_SERVER_ERROR || id == EV_WS_CANCEL_DONE ||
           id == EV_WS_SPEECH_STARTED || id == EV_WS_SPEECH_STOPPED ||
           id == EV_WS_RESPONSE_DONE || id == EV_AUDIO_UPLOAD_FAILED;
}

static void main_task(void *arg)
{
    (void)arg;
    conversation_active = false;
    resume_deadline_ms = 0;
    ESP_LOGI(TAG, "Boot session=%08lx reset_reason=%d build=pcm-playback-v1; fresh wake required",
             (unsigned long)esp_random(), esp_reset_reason());
    app_state_set(APP_STATE_WAIT_WAKE);
    led_set_wifi_err();
    if (start_saved_wifi() != ESP_OK) enter_provisioning("no usable Wi-Fi configuration");

    while (1) {
        event_t event;
        if (event_bus_receive(s_bus, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (tagged_ws_event(event.type) &&
                ((uint32_t)(uintptr_t)event.payload != ws_service_generation() ||
                 !session_requested)) continue;
            app_state_t current_state = app_state_get();
            switch (event.type) {
            case EV_WIFI_GOT_IP:
                if (provisioning_service_is_active() || wifi_service_get_state() != WIFI_STATE_GOT_IP) break;
                provisioning_service_confirm();
                if (!mqtt_service_is_connected()) {
                    esp_err_t mqtt_err = mqtt_service_start();
                    if (mqtt_err != ESP_OK) ESP_LOGW(TAG, "MQTT start failed: %s", esp_err_to_name(mqtt_err));
                }
                /* 普通联网不打开 WS；重复 IP 事件不能打断正在进行的对话。 */
                if (!session_requested) {
                    ESP_LOGI(TAG, "Wi-Fi ready; waiting for wake word or REC before connecting");
                    app_state_back_to_wait_wake(false);
                    led_set_waiting_wake();
                }
                break;
            case EV_PROVISIONING_DONE:
                if ((uint32_t)(uintptr_t)event.payload == provisioning_service_generation())
                    leave_provisioning("credentials submitted");
                break;
            case EV_PROVISIONING_TIMEOUT:
                if (provisioning_service_is_active() && provisioning_service_timed_out())
                    leave_provisioning("10-minute timeout");
                break;
            case EV_MQTT_COMMAND: {
                const char *json = (const char *)event.payload;
                cJSON *root = json ? cJSON_Parse(json) : NULL;
                cJSON *type = root ? cJSON_GetObjectItem(root, "type") : NULL;
                if (cJSON_IsString(type) && strcmp(type->valuestring, "enter_provisioning") == 0)
                    enter_provisioning("MQTT command");
                if (root) cJSON_Delete(root);
                break;
            }
            case EV_WIFI_DISCONNECTED:
            case EV_WIFI_CONNECT_FAILED:
                /* 配网时以及已经恢复 IP 时，忽略队列中迟到的断线通知。 */
                if (provisioning_service_is_active() || wifi_service_get_state() == WIFI_STATE_GOT_IP ||
                    wifi_service_get_state() == WIFI_STATE_IDLE) break;
                if (event.type == EV_WIFI_CONNECT_FAILED && wifi_service_get_state() != WIFI_STATE_FAIL) break;
                (void)mqtt_service_stop();
                if (event.type == EV_WIFI_CONNECT_FAILED) {
                    provisioning_service_rollback();
                    enter_provisioning("Wi-Fi connection failed");
                } else {
                    end_conversation("Wi-Fi disconnected");
                }
                break;
            case EV_BUTTON_VOLUME_DOWN:
            case EV_BUTTON_VOLUME_UP:
                change_volume(event.type == EV_BUTTON_VOLUME_UP);
                break;
            case EV_BUTTON_TALK:
                if (current_state == APP_STATE_PLAYING) interrupt_response(true);
                else start_conversation("REC button");
                break;
            case EV_BUTTON_END_TALK:
                if (!provisioning_service_is_active()) end_conversation("REC held for 2s");
                break;
            case EV_BUTTON_PROVISION:
                if (provisioning_service_is_active()) leave_provisioning("SET held for 3s");
                else enter_provisioning("SET held for 3s");
                break;
            case EV_WS_CONNECTED:
                ESP_LOGI(TAG, "WebSocket transport connected; authenticating");
                auth_deadline_ms = now_ms() + 10000;
                if (ws_service_authenticate() != ESP_OK) recover_session("auth send failed");
                break;
            case EV_WS_AUTH_OK:
                auth_deadline_ms = retry_at_ms = 0;
                ESP_LOGI(TAG, "WebSocket authenticated; audio upload ready");
                conversation_active = true;
                resume_listening(false, resume_deadline_ms ? "reconnect" : "wake/REC connect complete");
                break;
            case EV_WS_DISCONNECTED:
                recover_session("WebSocket transport disconnected");
                break;
            case EV_WS_ERROR:
                recover_session("WebSocket transport/receive failure");
                break;
            case EV_WS_SERVER_ERROR:
                recover_session("server protocol error (see ws_service error code)");
                break;
            case EV_AUDIO_UPLOAD_FAILED:
                recover_session("audio upload failed");
                break;
            case EV_AUDIO_WAKE_DETECTED:
                start_conversation("wake word");
                break;
            case EV_AUDIO_SPEECH_START:
            case EV_WS_SPEECH_STARTED:
                if (conversation_active && current_state == APP_STATE_LISTENING)
                    app_state_reset_silence_timer();
                break;
            case EV_WS_SPEECH_STOPPED:
                if (!conversation_active || current_state != APP_STATE_LISTENING) break;
                ESP_LOGI(TAG, "Server speech_stopped; waiting for reply");
                app_state_pause_silence_timer();
                if (audio_in_stop(audio_in_get_handle()) != ESP_OK) {
                    recover_session("audio stop timeout");
                    break;
                }
                app_state_set(APP_STATE_PLAYING);
                response_deadline_ms = now_ms() + 45000;
                break;
            case EV_AUDIO_OUT_START:
                if (conversation_active && current_state == APP_STATE_PLAYING &&
                    ws_service_get_state() == WS_STATE_CONNECTED) {
                    response_deadline_ms = now_ms() + 180000;
                    led_set_speaking();
                }
                break;
            case EV_AUDIO_BARGE_IN:
                interrupt_response(false);
                break;
            case EV_WS_CANCEL_DONE:
                if (conversation_active && current_state == APP_STATE_CANCELLING) {
                    ESP_LOGI(TAG, "Cancel acknowledged; resume buffered microphone audio");
                    resume_listening(true, "barge-in completed");
                }
                break;
            case EV_WS_RESPONSE_DONE:
                if (conversation_active && current_state == APP_STATE_PLAYING) {
                    ESP_LOGI(TAG, "Reply complete; continue conversation");
                    resume_listening(false, "reply completed");
                }
                break;
            case EV_AUDIO_SILENCE_TIMEOUT:
                /* Ignore an already queued timeout from before playback. */
                if (current_state != APP_STATE_LISTENING) break;
                if (esp_timer_is_active(app_state_get_global()->silence_timer)) break;
                end_conversation("60s silence timeout");
                break;
            case EV_AUDIO_INPUT_ERROR:
                recover_session("audio input error");
                break;
            default: break;
            }
        }
        int64_t now = now_ms();
        save_volume_if_due(now);
        if (cancel_probe_ms && now >= cancel_probe_ms) {
            cancel_probe_ms = 0;
            ESP_LOGW(TAG, "Cancel confirmation delayed; keeping connection while waiting (limit 8 s)");
            ws_service_log_cancel_wait();
        }
        if (resume_deadline_ms && now >= resume_deadline_ms) {
            end_conversation("conversation recovery expired");
        }
        if (cancel_deadline_ms && now >= cancel_deadline_ms) {
            ws_service_log_cancel_wait();
            recover_session("cancel acknowledgement timed out");
        }
        else if (response_deadline_ms && now >= response_deadline_ms)
            recover_session("server reply timed out");
        else if (auth_deadline_ms && now >= auth_deadline_ms)
            recover_session("connection/authentication timed out");
        if (session_requested && retry_at_ms && now >= retry_at_ms &&
            wifi_service_get_state() == WIFI_STATE_GOT_IP && !provisioning_service_is_active()) {
            ESP_LOGI(TAG, "Connecting WebSocket (wake-triggered or retry) ...");
            /* Stop is idempotent, and retries a prior failed stop before start. */
            esp_err_t err = ws_service_disconnect();
            if (err == ESP_OK) err = ws_service_connect();
            if (err == ESP_OK) {
                retry_at_ms = 0;
                auth_deadline_ms = now_ms() + 20000;
            } else retry_at_ms = now_ms() + 5000;
        }
        led_service_poll(s_led, esp_log_timestamp());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
/* ── 应用入口 ───────────────────────────────────────────────── */

void app_main(void)
{
    s_bus = event_bus_create(32);
    if (s_bus == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        abort();
    }

    if (system_init() != ESP_OK) {
        ESP_LOGE(TAG, "system_init failed, rebooting in 3s");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    xTaskCreatePinnedToCore(main_task, "main", MAIN_TASK_STACK,
                            NULL, MAIN_TASK_PRIORITY, NULL, 0);

    ESP_LOGI(TAG, "app_main done, main_task running on core 0");
}
