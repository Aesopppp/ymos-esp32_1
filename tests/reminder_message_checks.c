/* 主机执行回归：直接编译 main/reminder_message.c 与工程锁定的 cJSON.c。
 * 不连接网络/开发板，不使用真实 API Key。运行方法见 docs/acos-reminders.md。 */
#include "../main/reminder_message.h"
#include "../main/reminder_playback_policy.h"
#include "../main/response_flow.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int64_t UNIX_NOW = 1793000000;
static const char *RECIPIENT = "device:test-key";
static const char *TOPIC = "acos/test-key/remind";
static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

static reminder_message_t *message(const char *id, int64_t timestamp)
{
    char json[512];
    snprintf(json, sizeof(json), "{\"v\":1,\"id\":\"%s\",\"ts\":%lld,\"from\":\"reminder:1\","
        "\"to\":\"device:test-key\",\"type\":\"remind\",\"data\":{\"text\":\"test\"}}",
        id, (long long)timestamp);
    reminder_message_t *result = reminder_message_parse(json, strlen(json), RECIPIENT);
    CHECK(result != NULL);
    CHECK(result->payload_len == strlen(json));
    CHECK(memcmp(result->payload, json, strlen(json) + 1) == 0);
    return result;
}

static void parser_checks(void)
{
    const char *json = " \n{\"v\":1,\"id\":\"test\",\"ts\":1793000000,\"from\":\"reminder:3\","
        "\"to\":\"device:test-key\",\"type\":\"scheduled_reminder\",\"data\":{\"reminder_id\":3,"
        "\"text\":\"hello\\nworld\",\"scheduled_for\":\"2026-10-26T11:00:00Z\"}}\r\n ";
    reminder_message_t *m = reminder_message_parse(json, strlen(json), RECIPIENT);
    CHECK(m && m->payload_len == strlen(json));
    CHECK(memcmp(m->payload, json, strlen(json) + 1) == 0); /* 包括空白及转义 */
    free(m);
    CHECK(!reminder_message_parse(json, strlen(json), "device:other"));
    const char *invalid[] = {
        "{}", "[]", "null", "{", "{}{}", "{\"v\":1,\"v\":1}",
        "{\"v\":1,\"id\":\"x\\u0000y\"}", "[[[[[[[[[0]]]]]]]]]"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(!reminder_message_parse(invalid[i], strlen(invalid[i]), RECIPIENT));
    /* 从合法消息修改，证明拒绝条件来自对应字段而非其它必填项缺失。 */
    m = message("parse", UNIX_NOW);
    char copy[1024];
    strcpy(copy, m->payload);
    char *at = strstr(copy, "\"v\":1"); at[4] = '2';
    CHECK(!reminder_message_parse(copy, strlen(copy), RECIPIENT));
    strcpy(copy, m->payload); at = strstr(copy, "\"ts\":"); at[5] = '-';
    CHECK(!reminder_message_parse(copy, strlen(copy), RECIPIENT));
    strcpy(copy, m->payload); at = strstr(copy, "remind\""); at[0] = 'X';
    CHECK(!reminder_message_parse(copy, strlen(copy), RECIPIENT));
    strcpy(copy, m->payload); at = strstr(copy, "test\""); at[0] = 0;
    CHECK(!reminder_message_parse(copy, m->payload_len, RECIPIENT));
    snprintf(copy, sizeof(copy), "%.*s,\"id\":\"duplicate\"}", (int)m->payload_len - 1, m->payload);
    CHECK(!reminder_message_parse(copy, strlen(copy), RECIPIENT));
    strcpy(copy, m->payload); at = strstr(copy, "\"text\":\"test\"");
    strcpy(at, "\"text\":\"test\",\"text\":\"again\"}}");
    CHECK(!reminder_message_parse(copy, strlen(copy), RECIPIENT));
    strcpy(copy, m->payload); at = strstr(copy, "\"text\":\"test\"");
    strcpy(at, "\"text\":\"test\",\"extra\":\"\\u0000\"}}");
    CHECK(!reminder_message_parse(copy, strlen(copy), RECIPIENT));
    free(m);
    CHECK(reminder_api_key_valid("sk-test_1.2"));
    CHECK(!reminder_api_key_valid(""));
    CHECK(!reminder_api_key_valid("other/#"));
    CHECK(reminder_broker_uri_valid("mqtt://127.0.0.1:1883"));
    CHECK(reminder_broker_uri_valid("mqtts://broker.example:8883"));
    CHECK(!reminder_broker_uri_valid("mqtt://user:password@host:1883"));
    CHECK(!reminder_broker_uri_valid("http://host"));
    CHECK(!reminder_credential_valid("test\r\ninjected"));
    CHECK(reminder_credential_valid(""));
}

static void fragment_checks(void)
{
    reminder_fragments_t fragments = {0};
    reminder_message_t *m = message("fragment", UNIX_NOW);
    size_t len = m->payload_len;
    /* 任意切分点、后续分片没有 topic；拼完必须与原始正文完全相同。 */
    for (size_t split = 1; split < len; ++split) {
        CHECK(!reminder_fragments_append(&fragments, TOPIC, TOPIC, strlen(TOPIC),
                                         m->payload, split, len, 0, false));
        CHECK(reminder_fragments_append(&fragments, TOPIC, NULL, 0,
                                         m->payload + split, len - split, len, split, false));
        CHECK(memcmp(fragments.bytes, m->payload, len + 1) == 0);
        reminder_fragments_reset(&fragments);
    }
    CHECK(!reminder_fragments_append(&fragments, TOPIC, TOPIC, strlen(TOPIC), "x", 1, 2, 0, true));
    CHECK(!fragments.bytes);
    CHECK(!reminder_fragments_append(&fragments, TOPIC, "wrong", 5, "x", 1, 1, 0, false));
    CHECK(!reminder_fragments_append(&fragments, TOPIC, TOPIC, strlen(TOPIC), "x", 1,
                                     REMINDER_MAX_BYTES + 1, 0, false));
    CHECK(!reminder_fragments_append(&fragments, TOPIC, TOPIC, strlen(TOPIC), "ab", 2, 5, 0, false));
    CHECK(!reminder_fragments_append(&fragments, TOPIC, NULL, 0, "cd", 2, 5, 3, false));
    CHECK(!fragments.bytes); /* 缺片/乱序清理 */
    CHECK(!reminder_fragments_append(&fragments, TOPIC, TOPIC, strlen(TOPIC), "a", 1, 3, 0, false));
    CHECK(reminder_fragments_append(&fragments, TOPIC, TOPIC, strlen(TOPIC), "xy", 2, 2, 0, false));
    CHECK(strcmp(fragments.bytes, "xy") == 0); /* 新消息替换未完成分片 */
    reminder_fragments_reset(&fragments);
    free(m);
}

static void queue_checks(void)
{
    reminder_queue_t q = {0};
    CHECK(reminder_queue_enqueue(&q, message("a", UNIX_NOW), 100, UNIX_NOW) == REMINDER_QUEUED);
    CHECK(reminder_queue_enqueue(&q, message("a", UNIX_NOW), 200, UNIX_NOW) == REMINDER_DUPLICATE);
    CHECK(strcmp(reminder_queue_take(&q, 300, UNIX_NOW)->id, "a") == 0);
    CHECK(!reminder_queue_take(&q, 301, UNIX_NOW));
    CHECK(reminder_queue_enqueue(&q, message("a", UNIX_NOW), 302, UNIX_NOW) == REMINDER_DUPLICATE);
    reminder_queue_finish(&q, REMINDER_COMPLETED, 400);
    CHECK(reminder_queue_enqueue(&q, message("a", UNIX_NOW), 500, UNIX_NOW) == REMINDER_DUPLICATE);
    CHECK(reminder_queue_enqueue(&q, message("b", UNIX_NOW), 500, UNIX_NOW) == REMINDER_QUEUED);
    CHECK(reminder_queue_take(&q, 500, UNIX_NOW));
    reminder_queue_finish(&q, REMINDER_FAILED, 500);
    CHECK(reminder_queue_enqueue(&q, message("b", UNIX_NOW), 501, UNIX_NOW) == REMINDER_QUEUED);
    CHECK(reminder_queue_take(&q, 501, UNIX_NOW));
    reminder_queue_finish(&q, REMINDER_CANCELLED, 502);
    CHECK(reminder_queue_enqueue(&q, message("b", UNIX_NOW), 503, UNIX_NOW) == REMINDER_DUPLICATE);
    CHECK(reminder_queue_enqueue(&q, message("old", UNIX_NOW - 601), 1000, UNIX_NOW) == REMINDER_EXPIRED);
    CHECK(reminder_queue_enqueue(&q, message("future", UNIX_NOW + 301), 1000, UNIX_NOW) == REMINDER_EXPIRED);
    for (unsigned i = 0; i < REMINDER_MAX_PENDING; ++i) {
        char id[16]; snprintf(id, sizeof(id), "pending-%u", i);
        CHECK(reminder_queue_enqueue(&q, message(id, UNIX_NOW), 1000, UNIX_NOW) == REMINDER_QUEUED);
    }
    CHECK(reminder_queue_take(&q, 1001, UNIX_NOW));
    CHECK(reminder_queue_enqueue(&q, message("overflow", UNIX_NOW), 1001, UNIX_NOW) == REMINDER_FULL);
    reminder_queue_finish(&q, REMINDER_COMPLETED, 1002);
    for (unsigned i = 1; i < REMINDER_MAX_PENDING; ++i) {
        char id[16]; snprintf(id, sizeof(id), "pending-%u", i);
        CHECK(strcmp(reminder_queue_take(&q, 1003, UNIX_NOW)->id, id) == 0);
        reminder_queue_finish(&q, REMINDER_COMPLETED, 1003);
    }
    CHECK(!reminder_queue_take(&q, 1004, UNIX_NOW));
    CHECK(reminder_queue_enqueue(&q, message("ttl", UNIX_NOW), 2000, 0) == REMINDER_QUEUED);
    CHECK(!reminder_queue_take(&q, 2000 + REMINDER_QUEUE_TTL_MS, 0));
    /* 无可靠 Unix 校时仍有单调 TTL；一小时后可再次接受同 ID。 */
    CHECK(reminder_queue_enqueue(&q, message("a", UNIX_NOW),
                                 400 + REMINDER_SEEN_TTL_MS, 0) == REMINDER_QUEUED);
    reminder_queue_reset(&q);
    CHECK(!q.count && !q.active);
}

static void playback_checks(void)
{
    CHECK(!reminder_playback_expired(29999, 0, 0, false));
    CHECK(reminder_playback_expired(30000, 0, 0, false));
    CHECK(!reminder_playback_expired(299999, 0, 299999, false));
    CHECK(reminder_playback_expired(300000, 0, 300000, false));
    CHECK(!reminder_playback_expired(310000, 0, 1000, true)); /* EOF 后等尾音 */
    CHECK(reminder_playback_expired(360000, 0, 350000, true));
    CHECK(!reminder_playback_expired(100, UINT32_MAX - 100, UINT32_MAX - 100, false));
    CHECK(reminder_playback_expired(29900, UINT32_MAX - 100, UINT32_MAX - 100, false));
    response_phase_t phase = RESPONSE_IDLE;
    CHECK(!response_accept_audio(&phase)); /* 待机不接受未请求语音 */
    response_expect(&phase); /* 请求前开放接收门，不需要 speech_stopped */
    CHECK(response_accept_audio(&phase));
    CHECK(!response_playback_finished(phase, true, false));
    CHECK(response_begin_cancel(&phase));
    CHECK(!response_accept_audio(&phase));
    CHECK(response_finish(&phase) == RESPONSE_END_CANCELLED);
    CHECK(!response_accept_audio(&phase));
}

int main(void)
{
    parser_checks(); fragment_checks(); queue_checks(); playback_checks();
    printf("PASS: %u reminder checks (production parser/fragments/queue/playback policy)\n", checks);
    return 0;
}
