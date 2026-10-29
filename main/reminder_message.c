#include "reminder_message.h"
#include "cJSON.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

/* 大消息只占 PSRAM；失败时不挤占 Wi-Fi/TLS/音频 DMA 所需的内部 RAM。 */
static void *message_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

static bool has_control(const char *value)
{
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p)
        if (*p < 32 || *p == 127) return true;
    return false;
}

bool reminder_api_key_valid(const char *key)
{
    if (!key || !key[0] || strlen(key) > 256) return false;
    for (const unsigned char *p = (const unsigned char *)key; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return false;
    return true;
}

bool reminder_broker_uri_valid(const char *uri)
{
    if (!uri || strlen(uri) > 256 || has_control(uri) || strpbrk(uri, " \\@?#")) return false;
    size_t prefix = strncmp(uri, "mqtt://", 7) == 0 ? 7 :
                    strncmp(uri, "mqtts://", 8) == 0 ? 8 : 0;
    return prefix && uri[prefix] && !strchr(uri + prefix, '/');
}

bool reminder_credential_valid(const char *value)
{
    return value && strlen(value) <= 256 && !strpbrk(value, "\r\n");
}

/* 递归解析前限深；拒绝解码 NUL，防止字段截断后改变收件人或 ID。 */
static bool safe_structure(const char *json, size_t len)
{
    int depth = 0;
    bool quoted = false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)json[i];
        if (!c || (quoted && c < 32)) return false;
        if (quoted && c == '\\') {
            if (++i == len) return false;
            if (json[i] == 'u' && len - i >= 5 && memcmp(json + i + 1, "0000", 4) == 0)
                return false;
        } else if (c == '"') quoted = !quoted;
        else if (!quoted) {
            if (c == '{' || c == '[') { if (++depth > 8) return false; }
            else if ((c == '}' || c == ']') && --depth < 0) return false;
        }
    }
    return !quoted && depth == 0;
}

static bool unique_keys(const cJSON *node)
{
    for (const cJSON *child = node->child; child; child = child->next) {
        if (cJSON_IsObject(node)) {
            for (const cJSON *other = child->next; other; other = other->next)
                if (strcmp(child->string, other->string) == 0) return false;
        }
        if (!unique_keys(child)) return false;
    }
    return true;
}

static const char *string_field(const cJSON *object, const char *key, size_t max_len)
{
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(value) && value->valuestring[0] && strlen(value->valuestring) <= max_len
               ? value->valuestring : NULL;
}

static bool integer_field(const cJSON *value, double min, double max)
{
    return cJSON_IsNumber(value) && isfinite(value->valuedouble) &&
           value->valuedouble >= min && value->valuedouble <= max &&
           floor(value->valuedouble) == value->valuedouble;
}

reminder_message_t *reminder_message_parse(const char *json, size_t len, const char *recipient)
{
    if (!json || !recipient || !len || len > REMINDER_MAX_BYTES || !safe_structure(json, len))
        return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    reminder_message_t *message = NULL;
    if (!root) return NULL;
    while (end < json + len && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (end != json + len || !cJSON_IsObject(root) || !unique_keys(root)) goto done;
    const char *id = string_field(root, "id", 96);
    const char *from = string_field(root, "from", 128);
    const char *to = string_field(root, "to", 280);
    const char *type = string_field(root, "type", 32);
    const cJSON *ts = cJSON_GetObjectItemCaseSensitive(root, "ts");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!integer_field(cJSON_GetObjectItemCaseSensitive(root, "v"), 1, 1) ||
        !integer_field(ts, 1, 4102444800.0) || !id || !from || !to || !type ||
        has_control(id) || has_control(from) || strcmp(to, recipient) != 0 || !cJSON_IsObject(data))
        goto done;
    bool scheduled = strcmp(type, "scheduled_reminder") == 0;
    if (!scheduled && strcmp(type, "remind") != 0) goto done;
    if (!string_field(data, "text", 1024)) goto done;
    if (scheduled && (!integer_field(cJSON_GetObjectItemCaseSensitive(data, "reminder_id"),
                                    1, 9007199254740991.0) ||
                      !string_field(data, "scheduled_for", 64))) goto done;
    message = message_alloc(sizeof(*message) + len + 1);
    if (message) {
        strcpy(message->id, id);
        message->timestamp = (int64_t)ts->valuedouble;
        message->received_ms = 0;
        message->payload_len = len;
        memcpy(message->payload, json, len);
        message->payload[len] = 0;
    }
done:
    cJSON_Delete(root);
    return message;
}

void reminder_fragments_reset(reminder_fragments_t *buffer)
{
    free(buffer->bytes);
    memset(buffer, 0, sizeof(*buffer));
}

bool reminder_fragments_append(reminder_fragments_t *buffer, const char *expected_topic,
                               const char *topic, size_t topic_len, const char *data,
                               size_t len, size_t total, size_t offset, bool retained)
{
    if (!offset) reminder_fragments_reset(buffer);
    bool topic_matches = topic && topic_len == strlen(expected_topic) &&
                         memcmp(topic, expected_topic, topic_len) == 0;
    if (retained || !data || !len || !total || total > REMINDER_MAX_BYTES ||
        offset > total || len > total - offset || (!offset && !topic_matches) ||
        (offset && topic_len && !topic_matches)) goto reject;
    if (!offset) {
        buffer->bytes = message_alloc(total + 1);
        if (!buffer->bytes) goto reject;
        buffer->total = total;
    }
    if (!buffer->bytes || total != buffer->total || offset != buffer->received) goto reject;
    memcpy(buffer->bytes + offset, data, len);
    buffer->received += len;
    buffer->bytes[buffer->received] = 0;
    return buffer->received == buffer->total;
reject:
    reminder_fragments_reset(buffer);
    return false;
}

static bool expired(const reminder_message_t *message, int64_t now_ms, int64_t unix_seconds)
{
    return now_ms - message->received_ms >= REMINDER_QUEUE_TTL_MS ||
           (unix_seconds >= 1577836800 && (message->timestamp < unix_seconds - 600 ||
                                         message->timestamp > unix_seconds + 300));
}

static void prune(reminder_queue_t *queue, int64_t now_ms, int64_t unix_seconds)
{
    size_t kept = 0;
    for (size_t i = 0; i < queue->count; ++i) {
        if (expired(queue->pending[i], now_ms, unix_seconds)) free(queue->pending[i]);
        else queue->pending[kept++] = queue->pending[i];
    }
    queue->count = kept;
    for (size_t i = 0; i < REMINDER_MAX_SEEN; ++i)
        if (queue->seen[i].id[0] && now_ms - queue->seen[i].finished_ms >= REMINDER_SEEN_TTL_MS)
            queue->seen[i].id[0] = 0;
}

reminder_enqueue_result_t reminder_queue_enqueue(reminder_queue_t *queue,
    reminder_message_t *message, int64_t now_ms, int64_t unix_seconds)
{
    prune(queue, now_ms, unix_seconds);
    reminder_enqueue_result_t result = REMINDER_DUPLICATE;
    if (queue->active && strcmp(queue->active->id, message->id) == 0) goto reject;
    for (size_t i = 0; i < queue->count; ++i)
        if (strcmp(queue->pending[i]->id, message->id) == 0) goto reject;
    for (size_t i = 0; i < REMINDER_MAX_SEEN; ++i)
        if (strcmp(queue->seen[i].id, message->id) == 0) goto reject;
    message->received_ms = now_ms;
    if (expired(message, now_ms, unix_seconds)) { result = REMINDER_EXPIRED; goto reject; }
    if (queue->count + (queue->active != NULL) >= REMINDER_MAX_PENDING) {
        result = REMINDER_FULL; goto reject;
    }
    queue->pending[queue->count++] = message;
    return REMINDER_QUEUED;
reject:
    free(message);
    return result;
}

const reminder_message_t *reminder_queue_take(reminder_queue_t *queue,
    int64_t now_ms, int64_t unix_seconds)
{
    prune(queue, now_ms, unix_seconds);
    if (queue->active || !queue->count) return NULL;
    queue->active = queue->pending[0];
    --queue->count;
    memmove(queue->pending, queue->pending + 1, queue->count * sizeof(queue->pending[0]));
    return queue->active;
}

void reminder_queue_finish(reminder_queue_t *queue, reminder_outcome_t outcome, int64_t now_ms)
{
    if (!queue->active) return;
    /* 失败不记作成功，不自动重播半段音频；平台可用同一 ID 再投递。 */
    if (outcome != REMINDER_FAILED) {
        size_t index = queue->seen_next++ % REMINDER_MAX_SEEN;
        strcpy(queue->seen[index].id, queue->active->id);
        queue->seen[index].finished_ms = now_ms;
    }
    free(queue->active);
    queue->active = NULL;
}

void reminder_queue_reset(reminder_queue_t *queue)
{
    for (size_t i = 0; i < queue->count; ++i) free(queue->pending[i]);
    free(queue->active);
    memset(queue, 0, sizeof(*queue));
}
