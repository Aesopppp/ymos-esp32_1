#include "response_stream.h"
#include "reply_boundary.h"
#include <string.h>

static bool active(const response_stream_t *stream)
{
    return stream->phase == RESPONSE_WAITING || stream->phase == RESPONSE_PLAYING;
}

static bool cancelled(const proto_msg_t *msg)
{
    return strcmp(msg->status, "cancelled") == 0 || strcmp(msg->status, "canceled") == 0 ||
           strcmp(msg->reason, "cancelled") == 0 || strcmp(msg->reason, "canceled") == 0;
}

bool response_stream_cancel_boundary(const proto_msg_t *msg)
{
    return msg->evt == PROTO_EVT_RESPONSE_DONE ||
        (msg->evt == PROTO_EVT_RESPONSE_TERMINAL &&
         (cancelled(msg) || (strcmp(msg->status, "completed") == 0 && !msg->has_tool_call)));
}

bool response_stream_starts_turn(const proto_msg_t *msg, bool reminder, bool cancelling)
{
    /* 等待取消确认时，旧语音的迟到 speech_stopped 不能挪动回合号，
     * 否则随后真正的取消结束事件也会被误判成另一个回合。 */
    return msg->evt == PROTO_EVT_SPEECH_STOPPED && !reminder && !cancelling;
}

void response_stream_reset(response_stream_t *stream)
{
    memset(stream, 0, sizeof(*stream));
    stream->phase = RESPONSE_IDLE;
}

void response_stream_begin(response_stream_t *stream, uint32_t turn)
{
    response_stream_reset(stream);
    stream->turn = turn;
    stream->continuation_allowed = true;
    response_expect(&stream->phase);
}

void response_stream_user_started(response_stream_t *stream)
{
    /* 已进入下一次用户发言后，旧 response.created 不得重新关闭麦克风。 */
    stream->continuation_allowed = false;
}

bool response_stream_accept_audio(response_stream_t *stream, uint32_t turn)
{
    if (!reply_packet_current(stream->turn, turn) || !response_accept_audio(&stream->phase))
        return false;
    /* V1 不保证发送 created/message；当前回合的 raw Opus 本身也表示工具后续
     * 语音已开始。仅在接受音频后解除工具等待，旧回合/取消/丢弃均不能走到这里。
     * 第一段已 EOF 时，继续同一 FIFO 的后续音频，等待本段新的 audio.done。 */
    stream->tool_pending = false;
    stream->audio_eof = false;
    return true;
}

static bool matches_id(const response_stream_t *stream, const proto_msg_t *msg)
{
    /* V1 的 created/ID 都不是必填；无 created 时不能给无标识二进制强加 ID。
     * 一旦收到明确的 created，则有 ID 的旧结束事件必须与当前阶段严格匹配。 */
    return !stream->id_from_created || !msg->response_id[0] ||
           strcmp(stream->response_id, msg->response_id) == 0;
}

bool response_stream_cancel_done(response_stream_t *stream)
{
    if (stream->phase != RESPONSE_CANCELLING) return false;
    if (response_finish(&stream->phase) != RESPONSE_END_CANCELLED) return false;
    stream->continuation_allowed = false;
    stream->tool_pending = false;
    stream->audio_eof = false;
    return true;
}

response_stream_action_t response_stream_control(response_stream_t *stream, uint32_t turn,
                                                  const proto_msg_t *msg)
{
    if (!reply_packet_current(stream->turn, turn)) return RESPONSE_STREAM_NONE;
    if (msg->evt == PROTO_EVT_RESPONSE_CREATED) {
        if (!msg->response_id[0] || stream->phase == RESPONSE_CANCELLING ||
            stream->phase == RESPONSE_DISCARD ||
            (!active(stream) && !stream->continuation_allowed) ||
            strcmp(stream->response_id, msg->response_id) == 0 ||
            strcmp(stream->retired_id, msg->response_id) == 0) return RESPONSE_STREAM_NONE;
        bool continued = stream->phase == RESPONSE_IDLE;
        strcpy(stream->retired_id, stream->response_id);
        strcpy(stream->response_id, msg->response_id);
        stream->id_from_created = true;
        stream->tool_pending = stream->audio_eof = false;
        response_expect(&stream->phase);
        return continued ? RESPONSE_STREAM_CONTINUED : RESPONSE_STREAM_NONE;
    }
    if (!matches_id(stream, msg)) return RESPONSE_STREAM_NONE;
    if (stream->phase == RESPONSE_CANCELLING) {
        if (response_stream_cancel_boundary(msg) && response_stream_cancel_done(stream))
            return RESPONSE_STREAM_CANCELLED;
        return RESPONSE_STREAM_NONE;
    }
    if (!active(stream)) return RESPONSE_STREAM_NONE;
    if (msg->evt == PROTO_EVT_RESPONSE_TOOL ||
        (msg->evt == PROTO_EVT_RESPONSE_TERMINAL && msg->has_tool_call)) {
        stream->tool_pending = true;
        stream->audio_eof = false;
    } else if (msg->evt == PROTO_EVT_RESPONSE_MESSAGE) {
        stream->tool_pending = false;
        /* output_item.done 常在 audio.done 之后到达，只关闭消息项，
         * 不能撤销已经收到的音频 EOF；added 才表示下一段即将开始。 */
        if (strcmp(msg->raw_type, "response.output_item.added") == 0)
            stream->audio_eof = false;
    } else if (msg->evt == PROTO_EVT_RESPONSE_DONE) {
        if (!stream->id_from_created && msg->response_id[0])
            strcpy(stream->response_id, msg->response_id);
        stream->audio_eof = true;
        return RESPONSE_STREAM_AUDIO_END;
    }
    /* failed/incomplete 必须显式恢复；不能让设备无期限闪烁等待语音。
     * completed 只是阶段完成，既不保证收到音频，也不代表工具链最终完成。 */
    if (msg->evt == PROTO_EVT_RESPONSE_TERMINAL &&
        (cancelled(msg) || strcmp(msg->status, "failed") == 0 || strcmp(msg->status, "incomplete") == 0)) {
        stream->phase = RESPONSE_DISCARD;
        stream->continuation_allowed = false;
        return RESPONSE_STREAM_FAILED;
    }
    return RESPONSE_STREAM_NONE;
}

bool response_stream_finish_if_drained(response_stream_t *stream, bool drained,
                                       bool queue_empty, bool wire_audio_done)
{
    /* worker 不能在第一个 audio.done 之后越过 FIFO 中仍待处理的后续段。
     * wire_audio_done 防止 RX 已收到下一段、但尚未完成入队时抢先完成。 */
    if (stream->tool_pending || !queue_empty || !wire_audio_done ||
        !response_playback_finished(stream->phase, stream->audio_eof, drained)) return false;
    response_finish(&stream->phase);
    stream->audio_eof = false;
    return true;
}

bool response_stream_begin_cancel(response_stream_t *stream)
{
    stream->continuation_allowed = false;
    return response_begin_cancel(&stream->phase);
}
