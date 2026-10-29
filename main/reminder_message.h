/**
 * @file reminder_message.h
 * @brief ACOS 提醒消息的校验、分片和有界队列（不依赖 FreeRTOS）。
 *
 * 调用方负责串行化；服务层加锁，主机测试直接调用同一份生产实现。
 * 所有长度单位为字节，时间为单调毫秒；Unix 时间只用于已校时后的过期检查。
 */
#ifndef REMINDER_MESSAGE_H
#define REMINDER_MESSAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REMINDER_MAX_BYTES      8192u
#define REMINDER_MAX_PENDING    8u    /* 包含正在处理的一条 */
#define REMINDER_MAX_SEEN       128u
#define REMINDER_QUEUE_TTL_MS   (10LL * 60 * 1000)
#define REMINDER_SEEN_TTL_MS    (60LL * 60 * 1000)

typedef struct {
    char id[97];
    int64_t timestamp;
    int64_t received_ms;
    size_t payload_len;
    char payload[];              /* 原始 JSON；不得重新序列化后转发 */
} reminder_message_t;

typedef struct {
    char *bytes;
    size_t total, received;
} reminder_fragments_t;

typedef enum {
    REMINDER_QUEUED, REMINDER_DUPLICATE, REMINDER_FULL, REMINDER_EXPIRED
} reminder_enqueue_result_t;
typedef enum {
    REMINDER_COMPLETED, REMINDER_CANCELLED, REMINDER_FAILED
} reminder_outcome_t;

typedef struct {
    reminder_message_t *pending[REMINDER_MAX_PENDING];
    size_t count;
    reminder_message_t *active;
    struct { char id[97]; int64_t finished_ms; } seen[REMINDER_MAX_SEEN];
    size_t seen_next;
} reminder_queue_t;

bool reminder_api_key_valid(const char *key);
bool reminder_broker_uri_valid(const char *uri);
bool reminder_credential_valid(const char *value);

/* 成功时返回独立分配的消息，由 free() 或队列接管；失败不保留输入。 */
reminder_message_t *reminder_message_parse(const char *json, size_t len, const char *recipient);

/* 只接受连续分片、精确 topic 和非 retained 消息。完整时返回 true。
 * bytes 始终多分配一个 NUL；reset 后失效，调用方不得跨 reset 借用。 */
bool reminder_fragments_append(reminder_fragments_t *buffer, const char *expected_topic,
                               const char *topic, size_t topic_len, const char *data,
                               size_t len, size_t total, size_t offset, bool retained);
void reminder_fragments_reset(reminder_fragments_t *buffer);

/* queue 必须先清零。enqueue 无论结果如何都接管并释放/保留 message。 */
reminder_enqueue_result_t reminder_queue_enqueue(reminder_queue_t *queue,
    reminder_message_t *message, int64_t now_ms, int64_t unix_seconds);
/* 返回借用指针，直到 finish/reset；回调不得修改 active。 */
const reminder_message_t *reminder_queue_take(reminder_queue_t *queue,
    int64_t now_ms, int64_t unix_seconds);
void reminder_queue_finish(reminder_queue_t *queue, reminder_outcome_t outcome, int64_t now_ms);
void reminder_queue_reset(reminder_queue_t *queue);

#endif /* REMINDER_MESSAGE_H */
