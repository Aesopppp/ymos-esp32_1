#ifndef BUTTON_POLICY_H
#define BUTTON_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Korvo-2 六键共用 ADC1_CH4；单位 mV，来自乐鑫 esp-bsp 的 bsp_button.c。
 * MUTE/PLAY 仍需识别，以免未分配按键被误认为其他操作。多键组合不支持。
 */
typedef enum {
    BUTTON_NONE = 0, BUTTON_REC, BUTTON_MUTE, BUTTON_PLAY,
    BUTTON_SET, BUTTON_VOL_DOWN, BUTTON_VOL_UP, BUTTON_INVALID,
} button_id_t;

typedef enum {
    BUTTON_ACTION_NONE = 0, BUTTON_ACTION_VOL_DOWN, BUTTON_ACTION_VOL_UP,
    BUTTON_ACTION_TALK, BUTTON_ACTION_END_TALK, BUTTON_ACTION_PROVISION,
} button_action_t;

#define BUTTON_POLL_MS         10u
#define BUTTON_DEBOUNCE_MS     40u
#define BUTTON_REPEAT_START_MS 600u
#define BUTTON_REPEAT_MS       150u
#define BUTTON_END_TALK_MS     2000u
#define BUTTON_PROVISION_MS    3000u
#define BUTTON_VOLUME_STEP     5

#ifdef __cplusplus
#define BUTTON_CONSTEXPR constexpr
#else
#define BUTTON_CONSTEXPR
#endif

static inline BUTTON_CONSTEXPR button_id_t button_from_mv(int mv)
{
    if (mv >= 2310 && mv <= 2510) return BUTTON_REC;
    if (mv >= 1880 && mv <= 2080) return BUTTON_MUTE;
    if (mv >= 1550 && mv <= 1750) return BUTTON_PLAY;
    if (mv >= 1010 && mv <= 1210) return BUTTON_SET;
    if (mv >= 720 && mv <= 920) return BUTTON_VOL_DOWN;
    if (mv >= 280 && mv <= 480) return BUTTON_VOL_UP;
    /* 空闲高电平与电压区间之间的过渡/异常读数分开处理。 */
    return mv >= 2700 ? BUTTON_NONE : BUTTON_INVALID;
}

typedef struct {
    button_id_t candidate, stable;
    uint32_t candidate_since, pressed_at, repeat_at;
    bool blocked, long_sent;
} button_policy_t;

/* 上电先等松手；启动时按住按键不会直接执行长按或音量连发。 */
static inline BUTTON_CONSTEXPR button_policy_t button_policy_initial(void)
{
    button_policy_t policy = {BUTTON_INVALID, BUTTON_INVALID, 0, 0, 0, true, false};
    return policy;
}

static inline BUTTON_CONSTEXPR button_action_t button_volume_action(button_id_t key)
{
    return key == BUTTON_VOL_UP ? BUTTON_ACTION_VOL_UP : BUTTON_ACTION_VOL_DOWN;
}

/* 只输出动作，不接触 RTOS、音频和网络。uint32 差值允许毫秒计数自然回绕。 */
static inline BUTTON_CONSTEXPR button_action_t button_policy_feed(
    button_policy_t *policy, button_id_t sample, uint32_t now)
{
    if (sample != policy->candidate) {
        policy->candidate = sample;
        policy->candidate_since = now;
    }
    if ((uint32_t)(now - policy->candidate_since) < BUTTON_DEBOUNCE_MS) {
        return BUTTON_ACTION_NONE;
    }
    if (policy->stable != sample) {
        button_id_t previous = policy->stable;
        policy->stable = sample;
        if (sample == BUTTON_NONE) {
            bool talk = previous == BUTTON_REC && !policy->blocked && !policy->long_sent;
            policy->blocked = false;
            policy->long_sent = false;
            return talk ? BUTTON_ACTION_TALK : BUTTON_ACTION_NONE;
        }
        if (previous != BUTTON_NONE || sample == BUTTON_INVALID || policy->blocked) {
            policy->blocked = true;  /* 未稳定松手就换键：整个操作取消。 */
            return BUTTON_ACTION_NONE;
        }
        policy->pressed_at = now;
        policy->repeat_at = now;
        policy->long_sent = false;
        if (sample == BUTTON_VOL_UP || sample == BUTTON_VOL_DOWN) {
            return button_volume_action(sample);
        }
    }
    if (policy->blocked || sample == BUTTON_NONE || sample == BUTTON_INVALID) {
        return BUTTON_ACTION_NONE;
    }
    uint32_t held_ms = now - policy->pressed_at;
    if (sample == BUTTON_VOL_UP || sample == BUTTON_VOL_DOWN) {
        if (held_ms >= BUTTON_REPEAT_START_MS &&
            (uint32_t)(now - policy->repeat_at) >= BUTTON_REPEAT_MS) {
            policy->repeat_at = now;
            return button_volume_action(sample);
        }
    } else if (!policy->long_sent) {
        if (sample == BUTTON_REC && held_ms >= BUTTON_END_TALK_MS) {
            policy->long_sent = true;
            return BUTTON_ACTION_END_TALK;
        }
        if (sample == BUTTON_SET && held_ms >= BUTTON_PROVISION_MS) {
            policy->long_sent = true;
            return BUTTON_ACTION_PROVISION;
        }
    }
    return BUTTON_ACTION_NONE;
}

static inline BUTTON_CONSTEXPR int button_volume_next(int current, bool increase)
{
    int next = current + (increase ? BUTTON_VOLUME_STEP : -BUTTON_VOLUME_STEP);
    return next < 0 ? 0 : (next > 100 ? 100 : next);
}

#undef BUTTON_CONSTEXPR
#endif
