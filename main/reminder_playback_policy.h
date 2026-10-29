/** 提醒请求/音频流的超时策略；收到结束边界后允许 PCM 尾音继续排空。 */
#ifndef REMINDER_PLAYBACK_POLICY_H
#define REMINDER_PLAYBACK_POLICY_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
#define REMINDER_INLINE static constexpr
#else
#define REMINDER_INLINE static inline
#endif

REMINDER_INLINE bool reminder_playback_expired(uint32_t now_ms, uint32_t started_ms,
                                              uint32_t last_rx_ms, bool network_done)
{
    /* uint32_t 差值兼容约 49 天的时钟回绕；所有窗口远小于半周期。 */
    return network_done ? (uint32_t)(now_ms - started_ms) >= 360000u :
        ((uint32_t)(now_ms - started_ms) >= 300000u ||
         (uint32_t)(now_ms - last_rx_ms) >= 30000u);
}
#undef REMINDER_INLINE
#endif
