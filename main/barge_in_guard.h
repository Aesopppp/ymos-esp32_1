#ifndef BARGE_IN_GUARD_H
#define BARGE_IN_GUARD_H

#include <stdbool.h>
#include <stdint.h>

/* 仅用于播放期间的打断确认，不改变 WakeNet 或正常监听的 VAD。
 * 弱音/停顿不计入有效人声，但允许短暂间隙，避免一句话中的音节间隔清空进度。
 * 单次间隙与候选总时长同时设限，防止零散回声无限累积。
 * 这些是待真机验证的判定参数，不能证明 AEC 参考信号已经正确对齐。
 */
#define BARGE_IN_SAMPLE_RATE 16000u
#define BARGE_IN_GUARD_MS 1200u
#define BARGE_IN_MIN_ENERGY 12000
#define BARGE_IN_CONFIRM_MS 480u
#define BARGE_IN_MAX_GAP_MS 96u
#define BARGE_IN_WINDOW_MS 640u
#define BARGE_IN_CONFIRM_SAMPLES (BARGE_IN_SAMPLE_RATE * BARGE_IN_CONFIRM_MS / 1000u)
#define BARGE_IN_MAX_GAP_SAMPLES (BARGE_IN_SAMPLE_RATE * BARGE_IN_MAX_GAP_MS / 1000u)
#define BARGE_IN_WINDOW_SAMPLES (BARGE_IN_SAMPLE_RATE * BARGE_IN_WINDOW_MS / 1000u)
#define BARGE_IN_DIAGNOSTIC_ENERGY 2500

typedef struct {
    uint32_t samples;        /* 仅累计 VAD 人声且能量达标的采样数 */
    uint32_t window_samples; /* 从第一帧达标人声起，包含间隙的总采样数 */
    uint32_t gap_samples;    /* 当前连续不达标的采样数，恢复人声后清零 */
    int32_t min_energy;
    int32_t max_energy;
} barge_in_guard_t;

/* 明确返回未确认原因，由调用方限频记录；策略本身不操作日志、缓存或事件总线。 */
typedef enum {
    BARGE_IN_WAIT_STARTUP,
    BARGE_IN_INVALID_FRAME,
    BARGE_IN_WAIT_SPEECH,
    BARGE_IN_WAIT_ENERGY,
    BARGE_IN_COLLECTING,
    BARGE_IN_GAP_TOLERATED,
    BARGE_IN_GAP_EXPIRED,
    BARGE_IN_WINDOW_EXPIRED,
    BARGE_IN_CONFIRMED,
} barge_in_result_t;

#ifdef __cplusplus
#define BARGE_IN_CONSTEXPR constexpr
#else
#define BARGE_IN_CONSTEXPR
#endif

static inline BARGE_IN_CONSTEXPR uint32_t barge_in_samples_to_ms(uint32_t samples)
{
    return samples * 1000u / BARGE_IN_SAMPLE_RATE;
}

static inline BARGE_IN_CONSTEXPR void barge_in_guard_reset(barge_in_guard_t *guard)
{
    guard->samples = 0;
    guard->window_samples = 0;
    guard->gap_samples = 0;
    guard->min_energy = 0;
    guard->max_energy = 0;
}

static inline BARGE_IN_CONSTEXPR barge_in_result_t barge_in_guard_feed(
    barge_in_guard_t *guard, bool speech, int32_t energy, uint32_t samples,
    uint32_t playback_ms)
{
    if (playback_ms < BARGE_IN_GUARD_MS) {
        barge_in_guard_reset(guard);
        return BARGE_IN_WAIT_STARTUP;
    }
    if (samples == 0 || samples > BARGE_IN_WINDOW_SAMPLES) {
        barge_in_guard_reset(guard);
        return BARGE_IN_INVALID_FRAME;
    }

    const bool qualified_speech = speech && energy >= BARGE_IN_MIN_ENERGY;
    if (!guard->samples && !qualified_speech) {
        return speech ? BARGE_IN_WAIT_ENERGY : BARGE_IN_WAIT_SPEECH;
    }

    /* 640 ms 内必须累计 480 ms 有效人声，所有间隙合计最多占 160 ms。
     * 超时后从下一帧重新收集，不把间隔很远的几段人声拼成一次打断。
     */
    if (samples > BARGE_IN_WINDOW_SAMPLES - guard->window_samples) {
        barge_in_guard_reset(guard);
        return BARGE_IN_WINDOW_EXPIRED;
    }
    guard->window_samples += samples;

    if (!qualified_speech) {
        if (samples > BARGE_IN_MAX_GAP_SAMPLES - guard->gap_samples) {
            barge_in_guard_reset(guard);
            return BARGE_IN_GAP_EXPIRED;
        }
        guard->gap_samples += samples;
        return BARGE_IN_GAP_TOLERATED;
    }

    guard->gap_samples = 0;
    if (!guard->samples || energy < guard->min_energy) {
        guard->min_energy = energy;
    }
    if (energy > guard->max_energy) {
        guard->max_energy = energy;
    }
    const uint32_t remaining = BARGE_IN_CONFIRM_SAMPLES - guard->samples;
    guard->samples += samples < remaining ? samples : remaining;
    return guard->samples == BARGE_IN_CONFIRM_SAMPLES
           ? BARGE_IN_CONFIRMED : BARGE_IN_COLLECTING;
}

#undef BARGE_IN_CONSTEXPR
#endif
