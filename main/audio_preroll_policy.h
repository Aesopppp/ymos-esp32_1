#ifndef AUDIO_PREROLL_POLICY_H
#define AUDIO_PREROLL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

/* 预录音按 AFE 输出的采样数计时，不使用 ring buffer 的可写块大小估算占用。 */
#define AUDIO_PREROLL_SAMPLE_RATE 16000u
#define AUDIO_PREROLL_MS          1500u

#ifdef __cplusplus
#define AUDIO_PREROLL_CONSTEXPR constexpr
#else
#define AUDIO_PREROLL_CONSTEXPR
#endif

/* 为下一帧腾出位置。向上取整保留完整帧：512 samples 时为 47 帧 / 1504 ms。
 * 确认打断后的保护期覆盖 PLAYING -> CANCELLING -> LISTENING，直到补传成功。
 */
static inline AUDIO_PREROLL_CONSTEXPR size_t audio_preroll_frames_to_drop(
    size_t waiting_frames, size_t frame_samples, bool playing, bool protected_audio)
{
    if (!playing || protected_audio || frame_samples == 0) return 0;
    const size_t samples = AUDIO_PREROLL_SAMPLE_RATE * AUDIO_PREROLL_MS / 1000u;
    const size_t limit = (samples + frame_samples - 1) / frame_samples;
    return waiting_frames >= limit ? waiting_frames - limit + 1 : 0;
}

static inline AUDIO_PREROLL_CONSTEXPR size_t audio_preroll_samples_to_ms(size_t samples)
{
    return samples * 1000u / AUDIO_PREROLL_SAMPLE_RATE;
}

#undef AUDIO_PREROLL_CONSTEXPR
#endif
