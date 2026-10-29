#include "ws_service.h"
#include "audio_out.h"
#include "proto_service.h"
#include "response_stream.h"
#include "ws_rx_policy.h"
#include "reply_boundary.h"
#include "reminder_playback_policy.h"
#include "event_bus.h"
#include "cfg_service.h"
#include "board_config.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ws_service";
static esp_websocket_client_handle_t client;
static atomic_int state = WS_STATE_DISCONNECTED;
static atomic_uint generation = 1;
static bool client_started;
static char uri[256];
static uint8_t *frame;
static size_t frame_size, received;
static int opcode;
static QueueHandle_t rx_queue;
static SemaphoreHandle_t playback_lock;
static TaskHandle_t rx_task;
static response_stream_t reply; /* playback_lock 保护，包含音频段/工具阶段状态 */
static uint32_t rx_drop_count;
static atomic_size_t rx_bytes;
static atomic_uint wire_turn, wire_done_turn;
static uint32_t played_frames, discarded_frames;
static atomic_bool cancel_requested;
static atomic_bool rx_dispatching;
static uint32_t lifecycle_log_ms; /* worker 专用，限制扩展事件诊断频率 */
/* Scheduling hint only: never use this to discard audio. */
static atomic_bool cancel_waiting;
static atomic_int last_control_event;
static atomic_uint last_rx_ms;
static atomic_bool reminder_mode;
static uint32_t reminder_started_ms; /* playback_lock 保护 */
static atomic_uint reminder_rx_ms;
typedef struct {
    uint8_t *data;
    size_t len;
    int opcode;
    uint32_t generation;
    uint32_t turn;
} rx_frame_t;

static void publish(app_event_id_t id, uint32_t gen)
{
    /* Integer generation tag, not a borrowed pointer. */
    if (gen == atomic_load(&generation))
        event_bus_publish(id, (void *)(uintptr_t)gen, 0);
}
uint32_t ws_service_generation(void) { return atomic_load(&generation); }
static bool current(const rx_frame_t *packet)
{
    return packet->generation == atomic_load(&generation) &&
           (state == WS_STATE_CONNECTED || state == WS_STATE_AUTHING);
}

static void log_server_error(const proto_msg_t *msg)
{
    /* Do not dump JSON: the server may echo auth/input frames. */
    char message[193] = {0}, secret[256];
    size_t len = msg->error_msg_len > 0 ? (size_t)msg->error_msg_len : 0;
    if (len >= sizeof(message)) len = sizeof(message) - 1;
    if (len) memcpy(message, msg->error_msg, len);
    /* 提醒错误可能回显原始请求，其中 to 含企业 Key；只保留错误码。 */
    bool sensitive = atomic_load(&reminder_mode) || strstr(message, "sk-") || strstr(message, "token") ||
                     strstr(message, "password") || strstr(message, "Bearer");
    const cfg_key_t keys[] = { CFG_KEY_ACOS_TOKEN, CFG_KEY_WIFI_PASSWORD };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        cfg_service_get_str(keys[i], secret, sizeof(secret), "");
        if (secret[0] && strstr(message, secret)) sensitive = true;
    }
    for (size_t i = 0; i < len; ++i)
        if ((unsigned char)message[i] < 32) message[i] = ' ';
    /* Codes are bounded and allowed only as plain identifiers. */
    char code[65] = {0};
    int n = msg->error_code_text_len;
    if (n > 64) n = 64;
    for (int i = 0; i < n; ++i) {
        char c = msg->error_code_text[i];
        code[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') ? c : '?';
    }
    ESP_LOGW(TAG, "Server error: code=%d/%s message=%s", msg->error_code, code,
             sensitive ? "[redacted]" : message);
}
static bool cancel_already_done(const proto_msg_t *msg)
{
    /* A cancel racing response.audio.done is reported by some gateways only
     * as this message. It means the response is already stopped. */
    return ws_cancel_inactive(msg->error_code_text, msg->error_code_text_len,
                              msg->error_msg, msg->error_msg_len);
}

/* RX 回调只观察网络音频进度，不负责结束播放。completed != audio.done。
 * 工具/消息阶段由 worker 按 ID 校验后修改 audio_eof；不能在这里让迟到或
 * 被忽略的控制帧撤销当前边界。FIFO/rx_dispatching 保护尚未处理的阶段。 */
static void observe_control(const proto_msg_t *msg)
{
    atomic_store(&last_control_event, msg->evt);
    if (response_stream_starts_turn(msg, atomic_load(&reminder_mode), atomic_load(&cancel_requested))) {
        atomic_fetch_add(&wire_turn, 1);
        atomic_store(&wire_done_turn, 0);
    } else if (msg->evt == PROTO_EVT_RESPONSE_DONE) {
        atomic_store(&wire_done_turn, atomic_load(&wire_turn));
    }
}

/* 状态只输出固定类别，避免扩展字段带入业务正文。 */
static const char *lifecycle_status(const proto_msg_t *msg)
{
    if (!msg->status[0]) return "unspecified";
    if (strcmp(msg->status, "completed") == 0) return "completed";
    if (strcmp(msg->status, "failed") == 0) return "failed";
    if (strcmp(msg->status, "incomplete") == 0) return "incomplete";
    if (strcmp(msg->status, "cancelled") == 0 || strcmp(msg->status, "canceled") == 0)
        return "cancelled";
    return "other";
}

/* 不打印业务正文、工具参数或原始 ID；哈希仅供前后阶段关联。 */
static void log_lifecycle(const proto_msg_t *msg, response_stream_action_t action)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (action == RESPONSE_STREAM_NONE && (uint32_t)(now - lifecycle_log_ms) < 5000) return;
    lifecycle_log_ms = now;
    uint32_t id_hash = 0;
    for (const unsigned char *p = (const unsigned char *)msg->response_id; *p; ++p)
        id_hash = (id_hash ^ *p) * 16777619u;
    ESP_LOGI(TAG, "Response stage: turn=%lu event=%s id_hash=%08lx status=%s has_tool=%d "
             "action=%d phase=%d tool_pending=%d eof=%d decoded=%lu discarded=%lu queue=%u",
             (unsigned long)reply.turn, proto_event_name(msg->evt), (unsigned long)id_hash,
             lifecycle_status(msg), msg->has_tool_call, action, reply.phase, reply.tool_pending,
             reply.audio_eof, (unsigned long)played_frames, (unsigned long)discarded_frames,
             (unsigned)uxQueueMessagesWaiting(rx_queue));
}

static void release_rx_frame(uint8_t *data, size_t len)
{
    if (!data) return;
    free(data);
    atomic_fetch_sub(&rx_bytes, len + 1);
}

static void drop_audio(void)
{
    if ((++rx_drop_count & 31u) == 1u)
        ESP_LOGW(TAG, "Audio backlog limit: dropped %lu incoming frames; connection retained",
                 (unsigned long)rx_drop_count);
}

static void enqueue_rx_frame(rx_frame_t *packet)
{
    if (xQueueSend(rx_queue, packet, 0) == pdTRUE) return;
    release_rx_frame(packet->data, packet->len);
    if (packet->opcode == 2) drop_audio();
    else {
        /* Audio leaves reserved slots. A control-only overflow is a real
         * protocol failure: never silently lose an auth/end boundary. */
        ESP_LOGE(TAG, "Control queue exhausted; recovering session");
        publish(EV_WS_ERROR, packet->generation);
    }
}

static void handle_frame(const rx_frame_t *packet)
{
    if (!current(packet)) return;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (!current(packet)) { xSemaphoreGive(playback_lock); return; }
    if (packet->opcode == 2) {
        /* Decode/I2S on this worker, never on the client's locked RX task.
         * Cancel takes this same lock, so old audio cannot restart playback. */
        bool after_tool = reply.tool_pending;
        if (state == WS_STATE_CONNECTED &&
            response_stream_accept_audio(&reply, packet->turn)) {
            audio_out_set_streaming(true);
            if (audio_out_play_opus(packet->data, packet->len) < 0)
                publish(EV_WS_SERVER_ERROR, packet->generation);
            else {
                ++played_frames;
                /* 每回合首帧/工具续流首帧各记录一次，不逐帧打印。 */
                if (played_frames == 1 || after_tool)
                    ESP_LOGI(TAG, "Reply audio accepted: turn=%lu after_tool=%d decoded=%lu",
                             (unsigned long)reply.turn, after_tool, (unsigned long)played_frames);
            }
        } else if (reply_packet_current(reply.turn, packet->turn)) {
            ++discarded_frames;
        }
        xSemaphoreGive(playback_lock);
        return;
    }
    proto_msg_t msg;
    proto_result_t result = proto_parse_json((char *)packet->data, &msg);
    if (result != PROTO_OK) {
        if (result != PROTO_ERR_UNKNOWN_TYPE) {
            ESP_LOGW(TAG, "Invalid control frame: parse=%d bytes=%u", result, (unsigned)packet->len);
            publish(EV_WS_SERVER_ERROR, packet->generation);
        }
        xSemaphoreGive(playback_lock); return;
    }
    switch (msg.evt) {
    case PROTO_EVT_PROXY_CONNECTED:
        if (state != WS_STATE_AUTHING) break;
        state = WS_STATE_CONNECTED;
        publish(EV_WS_AUTH_OK, packet->generation);
        break;
    case PROTO_EVT_SPEECH_STARTED:
        if (atomic_load(&reminder_mode)) break;
        /* Speech detection does not acknowledge response.cancel. */
        response_stream_user_started(&reply);
        publish(EV_WS_SPEECH_STARTED, packet->generation);
        break;
    case PROTO_EVT_SPEECH_STOPPED:
        if (atomic_load(&reminder_mode)) break;
        if (reply.phase == RESPONSE_CANCELLING) break;
        response_stream_begin(&reply, packet->turn);
        played_frames = discarded_frames = 0;
        publish(EV_WS_SPEECH_STOPPED, packet->generation);
        break;
    case PROTO_EVT_RESPONSE_CREATED:
    case PROTO_EVT_RESPONSE_TOOL:
    case PROTO_EVT_RESPONSE_MESSAGE:
    case PROTO_EVT_RESPONSE_DONE:
    case PROTO_EVT_RESPONSE_TERMINAL: {
        response_stream_action_t action = response_stream_control(&reply, packet->turn, &msg);
        log_lifecycle(&msg, action);
        if (action == RESPONSE_STREAM_AUDIO_END) {
            audio_out_set_streaming(false);
        } else if (action == RESPONSE_STREAM_CANCELLED) {
            atomic_store(&cancel_requested, false);
            atomic_store(&reminder_mode, false);
            publish(EV_WS_CANCEL_DONE, packet->generation);
        } else if (action == RESPONSE_STREAM_FAILED) {
            audio_out_stop();
            publish(EV_WS_SERVER_ERROR, packet->generation);
        } else if (action == RESPONSE_STREAM_CONTINUED) {
            /* 仅明确的新 response.created 能恢复已排空回合；迟到二进制不能。 */
            publish(EV_WS_RESPONSE_CONTINUED, packet->generation);
        }
        break;
    }
    case PROTO_EVT_ERROR:
        if (cancel_already_done(&msg) && state == WS_STATE_CONNECTED) {
            ESP_LOGI(TAG, "Cancel already completed; keeping WebSocket connected");
            if (reply.phase == RESPONSE_CANCELLING) {
                response_stream_cancel_done(&reply);
                atomic_store(&cancel_requested, false);
                atomic_store(&reminder_mode, false);
                publish(EV_WS_CANCEL_DONE, packet->generation);
            }
        } else {
            log_server_error(&msg);
            /* Business error != transport disconnect. Main explicitly
             * recovers the session instead of poisoning connection state. */
            reply.phase = RESPONSE_DISCARD;
            publish(EV_WS_SERVER_ERROR, packet->generation);
        }
        break;
    default: break;
    }
    xSemaphoreGive(playback_lock);
}
static void finish_playback_if_drained(void)
{
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (state == WS_STATE_CONNECTED && !atomic_load(&rx_dispatching) &&
        response_stream_finish_if_drained(&reply, audio_out_is_drained(),
            uxQueueMessagesWaiting(rx_queue) == 0,
            reply_boundary_received(reply.turn, atomic_load(&wire_done_turn)))) {
        ESP_LOGI(TAG, "Playback drained: turn=%lu decoded=%lu discarded=%lu",
                 (unsigned long)reply.turn, (unsigned long)played_frames,
                 (unsigned long)discarded_frames);
        /* 空响应不能算播报成功，避免平台尚未接好语音时静默吞掉提醒。 */
        publish(atomic_load(&reminder_mode) && !played_frames ? EV_WS_SERVER_ERROR :
                EV_WS_RESPONSE_DONE, atomic_load(&generation));
    }
    xSemaphoreGive(playback_lock);
}
static void receive_task(void *arg)
{
    (void)arg;
    rx_frame_t packet;
    while (true) {
        if (xQueueReceive(rx_queue, &packet, pdMS_TO_TICKS(10)) == pdTRUE) {
            handle_frame(&packet);
            release_rx_frame(packet.data, packet.len);
        }
        finish_playback_if_drained();
        /* On two cores, let the waiting main task acquire playback_lock
         * between frames. Keep all queued audio until it commits cancel. */
        if (atomic_load(&cancel_waiting)) vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *event)
{
    (void)arg; (void)base;
    uint32_t gen = atomic_load(&generation);
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        state = WS_STATE_AUTHING;
        publish(EV_WS_CONNECTED, gen);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
        state = WS_STATE_DISCONNECTED;
        release_rx_frame(frame, frame_size); frame = NULL; frame_size = received = 0;
        publish(EV_WS_DISCONNECTED, gen);
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        state = WS_STATE_ERROR;
        esp_websocket_event_data_t *failure = event;
        if (failure) ESP_LOGW(TAG, "Transport detail: type=%d socket=%d tls=0x%x stack=%d close=%d",
                             failure->error_handle.error_type,
                             failure->error_handle.esp_transport_sock_errno,
                             failure->error_handle.esp_tls_last_esp_err,
                             failure->error_handle.esp_tls_stack_err, failure->close_status_code);
        publish(EV_WS_ERROR, gen);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        esp_websocket_event_data_t *data = event;
        if (data->op_code != 1 && data->op_code != 2) return;
        if (data->payload_len <= 0 || data->payload_len > 65536 ||
            data->data_len < 0 || data->payload_offset < 0) return;
        if (data->payload_offset == 0) {
            release_rx_frame(frame, frame_size); frame = NULL; received = 0;
            frame_size = data->payload_len; opcode = data->op_code;
            size_t used = atomic_load(&rx_bytes);
            if (opcode == 2 && !ws_rx_audio_fits(uxQueueMessagesWaiting(rx_queue),
                                                used, frame_size + 1)) {
                drop_audio();
                return;
            }
            if (used + frame_size + 1 <= WS_RX_AUDIO_BYTES + WS_RX_CONTROL_BYTES)
                frame = heap_caps_malloc(frame_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!frame) {
                if (opcode == 2) drop_audio();
                else publish(EV_WS_ERROR, gen);
                return;
            }
            atomic_fetch_add(&rx_bytes, frame_size + 1);
        }
        if (!frame || (size_t)data->payload_offset != received ||
            received + (size_t)data->data_len > frame_size) return;
        memcpy(frame + received, data->data_ptr, data->data_len);
        received += data->data_len;
        if (received == frame_size) {
            atomic_store(&rx_dispatching, true);
            frame[received] = 0;
            atomic_store(&last_rx_ms, (uint32_t)(esp_timer_get_time() / 1000));
            rx_frame_t packet = {.data=frame, .len=received, .opcode=opcode, .generation=gen};
            if (opcode == 2) atomic_store(&wire_done_turn, 0);
            if (opcode == 1) {
                proto_msg_t msg;
                proto_result_t result = proto_parse_json((char *)frame, &msg);
                if (result == PROTO_OK) observe_control(&msg);
                else if (atomic_load(&cancel_requested))
                    ESP_LOGW(TAG, "Unrecognized control while cancelling (parse=%d, bytes=%u)",
                             result, (unsigned)received);
            }
            if (atomic_load(&reminder_mode) &&
                (opcode == 2 || atomic_load(&wire_done_turn) == atomic_load(&wire_turn)))
                atomic_store(&reminder_rx_ms, (uint32_t)(esp_timer_get_time() / 1000));
            packet.turn = atomic_load(&wire_turn);
            enqueue_rx_frame(&packet);
            frame = NULL; frame_size = received = 0;
            atomic_store(&rx_dispatching, false);
        }
    }
}

esp_err_t ws_service_init(void)
{
    if (client) return ESP_OK;
    playback_lock = xSemaphoreCreateMutex();
    rx_queue = xQueueCreateWithCaps(WS_RX_FRAMES, sizeof(rx_frame_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!playback_lock || !rx_queue) goto fail;
    cfg_service_get_str(CFG_KEY_WS_URL, uri, sizeof(uri), ACOS_WS_URL);
    esp_websocket_client_config_t cfg = {
        .uri = uri, .disable_auto_reconnect = true, .network_timeout_ms = 10000,
        .task_stack = 6144, /* 有界 JSON 解析和协议字段副本，避免默认栈过紧。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* WebSocket-level keepalive: send a ping every 20s so server idle timeout
         * does not silently close the connection. PONG timeout 30s before we
         * declare the link dead. (esp_websocket_client v1.8.0) */
        .ping_interval_sec = 20,
        .pingpong_timeout_sec = 30,
        .disable_pingpong_discon = false,
        /* TCP-level keepalive (slower safety net). */
        .keep_alive_enable = true,
        .keep_alive_idle = 30,
        .keep_alive_interval = 10,
        .keep_alive_count = 3,
    };
    client = esp_websocket_client_init(&cfg);
    if (!client) goto fail;
    if (esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, on_ws, NULL) != ESP_OK) goto fail;
    if (xTaskCreatePinnedToCore(receive_task, "ws_rx_audio", 8192, NULL, 4, &rx_task, 1) != pdPASS) goto fail;
    ESP_LOGI(TAG, "RX buffer: %u frames, %u KiB Opus budget in PSRAM; %u control slots reserved",
             WS_RX_FRAMES, WS_RX_AUDIO_BYTES / 1024u, WS_RX_CONTROL_RESERVE);
    return ESP_OK;
fail:
    if (client) { esp_websocket_client_destroy(client); client = NULL; }
    if (rx_queue) { vQueueDeleteWithCaps(rx_queue); rx_queue = NULL; }
    if (playback_lock) { vSemaphoreDelete(playback_lock); playback_lock = NULL; }
    return ESP_ERR_NO_MEM;
}
esp_err_t ws_service_connect(void)
{
    if (!client) return ESP_ERR_INVALID_STATE;
    if (client_started) return ESP_OK;
    state = WS_STATE_CONNECTING;
    esp_err_t err = esp_websocket_client_start(client);
    if (err == ESP_OK) client_started = true;
    else state = WS_STATE_ERROR;
    return err;
}
esp_err_t ws_service_authenticate(void)
{
    if (!client || state != WS_STATE_AUTHING) return ESP_ERR_INVALID_STATE;
    char token[256], auth[512];
    cfg_service_get_str(CFG_KEY_ACOS_TOKEN, token, sizeof(token), "");
    if (!token[0]) { ESP_LOGE(TAG, "Configure ACOS token in NVS"); return ESP_ERR_INVALID_STATE; }
    size_t len = proto_build_auth_frame(auth, sizeof(auth), token);
    return len && esp_websocket_client_send_text(client, auth, len, pdMS_TO_TICKS(1000)) == (int)len
               ? ESP_OK : ESP_FAIL;
}
esp_err_t ws_service_disconnect(void)
{
    if (!client) return ESP_ERR_INVALID_STATE;
    state = WS_STATE_DISCONNECTED;
    if (client_started) {
        esp_err_t err = esp_websocket_client_stop(client);
        /* Component 1.8 returns ESP_FAIL if its task already stopped after
         * a transport failure (auto reconnect is disabled). That is success
         * for this idempotent main-task stop, not a reason to loop forever. */
        if (err != ESP_OK && err != ESP_FAIL) return err;
        client_started = false;
    }
    atomic_fetch_add(&generation, 1);
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    state = WS_STATE_DISCONNECTED;
    response_stream_reset(&reply);
    atomic_store(&rx_dispatching, false);
    atomic_store(&wire_turn, 0);
    atomic_store(&wire_done_turn, 0);
    atomic_store(&cancel_requested, false);
    atomic_store(&reminder_mode, false);
    audio_out_stop();
    xSemaphoreGive(playback_lock);
    rx_frame_t packet;
    while (xQueueReceive(rx_queue, &packet, 0) == pdTRUE) release_rx_frame(packet.data, packet.len);
    release_rx_frame(frame, frame_size); frame = NULL; frame_size = received = 0;
    rx_drop_count = 0;
    return ESP_OK;
}
esp_err_t ws_service_deinit(void)
{
    if (!client) return ESP_OK;
    esp_err_t err = ws_service_disconnect();
    if (err != ESP_OK) return err;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (rx_task) { vTaskDelete(rx_task); rx_task = NULL; }
    xSemaphoreGive(playback_lock);
    esp_websocket_client_destroy(client); client = NULL;
    vQueueDeleteWithCaps(rx_queue); rx_queue = NULL;
    vSemaphoreDelete(playback_lock); playback_lock = NULL;
    return ESP_OK;
}
ws_conn_state_t ws_service_get_state(void) { return state; }
esp_websocket_client_handle_t ws_service_get_client(void) { return client; }
esp_err_t ws_service_send_json(const char *text)
{
    if (!client || state != WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    if (!text) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(text);
    return esp_websocket_client_send_text(client, text, len, pdMS_TO_TICKS(1000)) == (int)len ? ESP_OK : ESP_FAIL;
}
esp_err_t ws_service_send_opus_frame(const uint8_t *data, size_t len)
{
    if (!data || !len || len > 65536) return ESP_ERR_INVALID_ARG;
    if (state != WS_STATE_CONNECTED || atomic_load(&reminder_mode)) return ESP_ERR_INVALID_STATE;
    size_t capacity = 4 * ((len + 2) / 3) + 1, written;
    char *encoded = malloc(capacity), *json = malloc(capacity + 256);
    if (!encoded || !json) { free(encoded); free(json); return ESP_ERR_NO_MEM; }
    esp_err_t err = ESP_FAIL;
    if (mbedtls_base64_encode((unsigned char *)encoded, capacity, &written, data, len) == 0) {
        encoded[written] = 0;
        if (proto_build_input_audio_append(json, capacity + 256, encoded, NULL)) err = ws_service_send_json(json);
    }
    free(encoded); free(json); return err;
}
esp_err_t ws_service_cancel_response(void)
{
    if (state != WS_STATE_CONNECTED) return ESP_ERR_INVALID_STATE;
    /* Do not skip any audio before holding this mutex and changing the phase.
     * Otherwise the worker can drain the tail and report a false NORMAL end
     * while the main task is still waiting to commit cancellation. */
    atomic_store(&cancel_waiting, true);
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    atomic_store(&cancel_waiting, false);
    if (state != WS_STATE_CONNECTED) {
        xSemaphoreGive(playback_lock);
        return ESP_ERR_INVALID_STATE;
    }
    bool pending = response_stream_begin_cancel(&reply);
    const uint32_t cancelled_turn = reply.turn;
    atomic_store(&cancel_requested, pending);
    if (pending) ESP_LOGI(TAG, "Cancel committed: turn=%lu played=%lu queued=%u",
                         (unsigned long)reply.turn, (unsigned long)played_frames,
                         (unsigned)uxQueueMessagesWaiting(rx_queue));
    if (pending && audio_out_stop() != ESP_OK) {
        xSemaphoreGive(playback_lock);
        return ESP_FAIL;
    }
    bool local_only = pending && reply.audio_eof && !reply.tool_pending &&
        !atomic_load(&rx_dispatching) && uxQueueMessagesWaiting(rx_queue) == 0 &&
        reply_boundary_received(reply.turn, atomic_load(&wire_done_turn));
    if (local_only) {
        response_stream_cancel_done(&reply);
        atomic_store(&reminder_mode, false);
        ESP_LOGI(TAG, "Barge-in: server already finished turn=%lu; discard local audio, keep connection",
                 (unsigned long)reply.turn);
        publish(EV_WS_CANCEL_DONE, atomic_load(&generation));
    }
    if (!pending || local_only) {
        atomic_store(&cancel_requested, false);
        atomic_store(&reminder_mode, false);
    }
    xSemaphoreGive(playback_lock);
    if (!pending) return ESP_ERR_NOT_FOUND;
    if (local_only) return ESP_OK;
    ESP_LOGI(TAG, "Barge-in: cancelling active server response turn=%lu", (unsigned long)cancelled_turn);
    return ws_service_send_json("{\"type\":\"response.cancel\"}");
}

void ws_service_log_cancel_wait(void)
{
    ESP_LOGW(TAG, "Cancel pending: wire_turn=%lu done_turn=%lu queue=%u bytes=%u last_control=%d rx_age_ms=%lu",
             (unsigned long)atomic_load(&wire_turn), (unsigned long)atomic_load(&wire_done_turn),
             (unsigned)uxQueueMessagesWaiting(rx_queue), (unsigned)atomic_load(&rx_bytes),
             atomic_load(&last_control_event),
             (unsigned long)((uint32_t)(esp_timer_get_time()/1000) - atomic_load(&last_rx_ms)));
}

void ws_service_log_response_wait(void)
{
    if (!playback_lock || !rx_queue) return;
    /* 主任务超时前取一次一致快照；不发送网络请求，不输出业务内容。 */
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    ESP_LOGW(TAG, "Reply pending: turn=%lu phase=%d tool_pending=%d eof=%d "
             "decoded=%lu discarded=%lu drained=%d queue=%u dispatching=%d wire_done=%d rx_age_ms=%lu",
             (unsigned long)reply.turn, reply.phase, reply.tool_pending, reply.audio_eof,
             (unsigned long)played_frames, (unsigned long)discarded_frames, audio_out_is_drained(),
             (unsigned)uxQueueMessagesWaiting(rx_queue), atomic_load(&rx_dispatching),
             reply_boundary_received(reply.turn, atomic_load(&wire_done_turn)),
             (unsigned long)((uint32_t)(esp_timer_get_time()/1000) - atomic_load(&last_rx_ms)));
    xSemaphoreGive(playback_lock);
}

esp_err_t ws_service_request_reminder(const char *payload)
{
    if (!payload || !payload[0]) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    if (state != WS_STATE_CONNECTED || reply.phase != RESPONSE_IDLE ||
        atomic_load(&reminder_mode) || !audio_out_is_drained()) {
        xSemaphoreGive(playback_lock);
        return ESP_ERR_INVALID_STATE;
    }
    /* 提醒没有 speech_stopped。先创建接收回合，再发送，避免快速下行被丢弃。
     * MQTT 消息已由服务层校验/持有，这里不解析或重新序列化。 */
    response_stream_begin(&reply, atomic_fetch_add(&wire_turn, 1) + 1);
    atomic_store(&wire_done_turn, 0);
    played_frames = discarded_frames = 0;
    reminder_started_ms = (uint32_t)(esp_timer_get_time() / 1000);
    atomic_store(&reminder_rx_ms, reminder_started_ms);
    atomic_store(&reminder_mode, true);
    xSemaphoreGive(playback_lock);
    /* 不持 playback_lock 等待网络；接收任务仍能处理快速到达的音频。 */
    esp_err_t err = ws_service_send_json(payload);
    if (err == ESP_OK) ESP_LOGI(TAG, "Reminder MQTT payload forwarded unchanged; waiting for Opus audio");
    return err; /* 失败由主任务断连清理，不能自动重播可能已发送的请求。 */
}

bool ws_service_reminder_timed_out(void)
{
    xSemaphoreTake(playback_lock, portMAX_DELAY);
    bool expired = atomic_load(&reminder_mode) && reminder_playback_expired(
        (uint32_t)(esp_timer_get_time() / 1000), reminder_started_ms,
        atomic_load(&reminder_rx_ms),
        reply.audio_eof && !reply.tool_pending &&
        reply_boundary_received(reply.turn, atomic_load(&wire_done_turn)));
    xSemaphoreGive(playback_lock);
    return expired;
}
