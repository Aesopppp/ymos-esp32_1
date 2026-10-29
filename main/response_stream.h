/** ACOS 回复流生命周期；由 ws_service 的 playback_lock 串行保护。
 * 本地 turn 隔离用户回合，response_id 隔离可识别的模型/工具阶段。
 * 二进制 Opus 无 ID，绝不凭迟到的音频重新打开已取消/已完成的回合。 */
#ifndef RESPONSE_STREAM_H
#define RESPONSE_STREAM_H

#include <stdint.h>
#include "proto_service.h"
#include "response_flow.h"

typedef struct {
    response_phase_t phase;
    uint32_t turn;
    bool audio_eof;
    bool tool_pending;
    bool continuation_allowed;
    bool id_from_created;
    char response_id[PROTO_RESPONSE_ID_SIZE];
    char retired_id[PROTO_RESPONSE_ID_SIZE];
} response_stream_t;

typedef enum {
    RESPONSE_STREAM_NONE,
    RESPONSE_STREAM_AUDIO_END,
    RESPONSE_STREAM_CANCELLED,
    RESPONSE_STREAM_FAILED,
    RESPONSE_STREAM_CONTINUED,
} response_stream_action_t;

void response_stream_reset(response_stream_t *stream);
void response_stream_begin(response_stream_t *stream, uint32_t turn);
void response_stream_user_started(response_stream_t *stream);
bool response_stream_accept_audio(response_stream_t *stream, uint32_t turn);
response_stream_action_t response_stream_control(response_stream_t *stream, uint32_t turn,
                                                  const proto_msg_t *msg);
bool response_stream_finish_if_drained(response_stream_t *stream, bool drained,
                                       bool queue_empty, bool wire_audio_done);
bool response_stream_begin_cancel(response_stream_t *stream);
bool response_stream_cancel_done(response_stream_t *stream);
/* 仅取消流程可接受 response.done completed；普通播放只由 audio.done 排空。 */
bool response_stream_cancel_boundary(const proto_msg_t *msg);
bool response_stream_starts_turn(const proto_msg_t *msg, bool reminder, bool cancelling);

#endif
