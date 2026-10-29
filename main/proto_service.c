/**
 * proto_service.c -- ACOS V1 protocol frame parser & builder.
 *
 *   按顶层事件、response、error 对象分别解析，避免工具结果中的 type/status
 *   被误当作协议字段。结果使用有界副本，释放 JSON 树后仍有效。
 *
 *   Frame builders are pure snprintf wrappers; Base64 encoding of Opus
 *   happens in ws_service.c (using mbedtls_base64_encode).
 */

#include "proto_service.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "cJSON.h"

#include "esp_random.h"
#include "esp_timer.h"

static proto_event_callback_t s_callback;

void proto_service_register_callback(proto_event_callback_t callback)
{
    s_callback = callback;
}

proto_result_t proto_dispatch_data(const char *data, size_t len)
{
    if (!data || !len || len == SIZE_MAX) return PROTO_ERR_INVALID_JSON;
    char *text = malloc(len + 1);
    if (!text) return PROTO_ERR_INVALID_JSON;
    memcpy(text, data, len);
    text[len] = 0;
    proto_msg_t msg;
    proto_result_t result = proto_parse_json(text, &msg);
    if (result == PROTO_OK && s_callback) s_callback(msg.evt);
    free(text);
    return result;
}

/* 先限制递归深度，再交给 cJSON 校验语法；拒绝解码 NUL，避免 ID 截断。 */
static bool safe_structure(const char *json)
{
    int depth = 0;
    bool quoted = false;
    for (const char *p = json; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (quoted && c < 32) return false;
        if (quoted && c == '\\') {
            if (!*++p) return false;
            if (*p == 'u' && strncmp(p + 1, "0000", 4) == 0) return false;
        } else if (c == '"') quoted = !quoted;
        else if (!quoted) {
            if (c == '{' || c == '[') { if (++depth > 12) return false; }
            else if ((c == '}' || c == ']') && --depth < 0) return false;
        }
    }
    return !quoted && depth == 0;
}

/* 同一对象中关键键重复时拒绝，而不是按字段顺序猜测含义。 */
static bool unique_keys(const cJSON *node)
{
    for (const cJSON *child = node->child; child; child = child->next) {
        if (cJSON_IsObject(node))
            for (const cJSON *other = child->next; other; other = other->next)
                if (strcmp(child->string, other->string) == 0) return false;
        if (!unique_keys(child)) return false;
    }
    return true;
}

static bool copy_field(const cJSON *object, const char *key, char *out, size_t size, int *len)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value || cJSON_IsNull(value)) return true;
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= size) return false;
    strcpy(out, value->valuestring);
    if (len) *len = (int)strlen(out);
    return true;
}

static bool tool_item(const cJSON *item)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
    return cJSON_IsString(type) &&
        (strcmp(type->valuestring, "function_call") == 0 || strcmp(type->valuestring, "tool_call") == 0);
}

proto_result_t proto_parse_json(const char *json_str, proto_msg_t *msg)
{
    if (!msg) return PROTO_ERR_INVALID_JSON;
    memset(msg, 0, sizeof(*msg));
    msg->error_code = -1;
    if (!json_str || !safe_structure(json_str)) return PROTO_ERR_INVALID_JSON;
    /* 不使用 cJSON 全局错误指针；网络回调和接收 worker 可并发解析。 */
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json_str, &end, true);
    if (!root) return PROTO_ERR_INVALID_JSON;
    proto_result_t result = PROTO_ERR_PARSE_FIELD;
    if (!cJSON_IsObject(root) || !unique_keys(root)) goto done;
    if (!copy_field(root, "type", msg->raw_type, sizeof(msg->raw_type), &msg->raw_type_len)) goto done;
    if (!msg->raw_type[0]) { result = PROTO_ERR_NO_TYPE; goto done; }
    const char *type = msg->raw_type;
    const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    if (response && !cJSON_IsObject(response)) goto done;
    if (!copy_field(root, "response_id", msg->response_id, sizeof(msg->response_id), NULL) ||
        !copy_field(root, "status", msg->status, sizeof(msg->status), &msg->status_len) ||
        !copy_field(root, "reason", msg->reason, sizeof(msg->reason), &msg->reason_len)) goto done;
    if (response) {
        char id[PROTO_RESPONSE_ID_SIZE] = {0};
        if (!copy_field(response, "id", id, sizeof(id), NULL)) goto done;
        if (id[0] && msg->response_id[0] && strcmp(id, msg->response_id) != 0) goto done;
        if (id[0]) strcpy(msg->response_id, id);
        if (!copy_field(response, "status", msg->status, sizeof(msg->status), &msg->status_len)) goto done;
        const cJSON *details = cJSON_GetObjectItemCaseSensitive(response, "status_details");
        if (details && !cJSON_IsNull(details) &&
            (!cJSON_IsObject(details) || !copy_field(details, "reason", msg->reason,
                                                   sizeof(msg->reason), &msg->reason_len))) goto done;
        const cJSON *output = cJSON_GetObjectItemCaseSensitive(response, "output");
        const cJSON *item;
        cJSON_ArrayForEach(item, output) if (tool_item(item)) msg->has_tool_call = true;
    }
    if (strcmp(type, "__proxy_connected__") == 0) {
        msg->evt = PROTO_EVT_PROXY_CONNECTED;
        if (!copy_field(root, "session_id", msg->session_id, sizeof(msg->session_id), &msg->session_id_len)) goto done;
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        msg->evt = PROTO_EVT_SPEECH_STARTED;
    } else if (strcmp(type, "input_audio_buffer.speech_stopped") == 0) {
        msg->evt = PROTO_EVT_SPEECH_STOPPED;
    } else if (strcmp(type, "response.audio.done") == 0) {
        msg->evt = PROTO_EVT_RESPONSE_DONE;
    } else if (strcmp(type, "response.done") == 0) {
        msg->evt = PROTO_EVT_RESPONSE_TERMINAL;
    } else if (strcmp(type, "response.cancelled") == 0 || strcmp(type, "response.canceled") == 0) {
        msg->evt = PROTO_EVT_RESPONSE_TERMINAL;
        strcpy(msg->status, "cancelled"); msg->status_len = 9;
    } else if (strcmp(type, "response.created") == 0) {
        msg->evt = PROTO_EVT_RESPONSE_CREATED;
        if (!msg->response_id[0]) goto done;
    } else if (strcmp(type, "response.output_item.added") == 0 || strcmp(type, "response.output_item.done") == 0) {
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "item");
        if (tool_item(item)) { msg->evt = PROTO_EVT_RESPONSE_TOOL; msg->has_tool_call = true; }
        else {
            const cJSON *item_type = cJSON_GetObjectItemCaseSensitive(item, "type");
            if (!cJSON_IsString(item_type) || strcmp(item_type->valuestring, "message") != 0) {
                result = PROTO_ERR_UNKNOWN_TYPE; goto done;
            }
            msg->evt = PROTO_EVT_RESPONSE_MESSAGE;
        }
    } else if (strcmp(type, "response.function_call_arguments.done") == 0) {
        msg->evt = PROTO_EVT_RESPONSE_TOOL; msg->has_tool_call = true;
    } else if (strcmp(type, "error") == 0) {
        msg->evt = PROTO_EVT_ERROR;
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        if (!error) error = root; /* 兼容旧网关的顶层错误字段。 */
        if (!cJSON_IsObject(error)) goto done;
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
        if (cJSON_IsNumber(code)) msg->error_code = code->valueint;
        else if (!copy_field(error, "code", msg->error_code_text, sizeof(msg->error_code_text), &msg->error_code_text_len)) goto done;
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");
        if (message && !cJSON_IsString(message)) goto done;
        if (cJSON_IsString(message)) {
            snprintf(msg->error_msg, sizeof(msg->error_msg), "%s", message->valuestring);
            msg->error_msg_len = (int)strlen(msg->error_msg);
        }
    } else { result = PROTO_ERR_UNKNOWN_TYPE; goto done; }
    result = PROTO_OK;
done:
    cJSON_Delete(root);
    return result;
}

void proto_msg_clear(proto_msg_t *msg) { if (msg) memset(msg, 0, sizeof(*msg)); }

/* ---- builders ---------------------------------------------------------- */
size_t proto_build_auth_frame(char *buf, size_t buf_size, const char *token)
{
    if (!buf || !token) return 0;
    int n = snprintf(buf, buf_size,
                     "{\"type\":\"auth\",\"token\":\"%s\"}",
                     token);
    return (n > 0 && (size_t)n < buf_size) ? (size_t)n : 0;
}

size_t proto_build_input_audio_append(char *buf, size_t buf_size,
                                       const char *base64_audio,
                                       const char *event_id)
{
    if (!buf || !base64_audio) return 0;

    char auto_id[32];
    const char *eid = event_id;
    if (!eid || eid[0] == '\0') {
        proto_make_event_id(auto_id, sizeof(auto_id));
        eid = auto_id;
    }
    int n = snprintf(buf, buf_size,
                     "{\"event_id\":\"%s\","
                     "\"type\":\"input_audio_buffer.append\","
                     "\"audio\":\"%s\","
                     "\"encoding\":\"opus\"}",
                     eid, base64_audio);
    return (n > 0 && (size_t)n < buf_size) ? (size_t)n : 0;
}

void proto_make_event_id(char *buf, size_t buf_size)
{
    if (!buf || buf_size < 24) return;
    uint64_t ms = (uint64_t)(esp_timer_get_time() / 1000);
    uint32_t rnd = esp_random();
    snprintf(buf, buf_size, "event_%llu_%06lx",
             (unsigned long long)ms, (unsigned long)(rnd & 0xFFFFFFu));
}

const char *proto_event_name(proto_event_t evt)
{
    switch (evt) {
        case PROTO_EVT_PROXY_CONNECTED: return "PROXY_CONNECTED";
        case PROTO_EVT_ERROR:           return "ERROR";
        case PROTO_EVT_SPEECH_STARTED:  return "SPEECH_STARTED";
        case PROTO_EVT_SPEECH_STOPPED:  return "SPEECH_STOPPED";
        case PROTO_EVT_RESPONSE_DONE:   return "RESPONSE_DONE";
        case PROTO_EVT_RESPONSE_TERMINAL:return "RESPONSE_TERMINAL";
        case PROTO_EVT_RESPONSE_CREATED: return "RESPONSE_CREATED";
        case PROTO_EVT_RESPONSE_TOOL:    return "RESPONSE_TOOL";
        case PROTO_EVT_RESPONSE_MESSAGE: return "RESPONSE_MESSAGE";
        default:                        return "UNKNOWN";
    }
}
