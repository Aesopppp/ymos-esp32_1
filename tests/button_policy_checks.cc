/* 编译期回归：直接调用生产按键策略；不验证 ADC 实物电压与任务调度。 */
#include "../main/button_policy.h"

namespace {
constexpr button_policy_t Released(uint32_t t = 0)
{
    auto p = button_policy_initial();
    button_policy_feed(&p, BUTTON_NONE, t);
    button_policy_feed(&p, BUTTON_NONE, t + 40);
    return p;
}

constexpr bool ShortTalk()
{
    auto p = Released();
    if (button_policy_feed(&p, BUTTON_REC, 100) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_REC, 139) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_REC, 140) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_NONE, 200) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_NONE, 239) != BUTTON_ACTION_NONE) return false;
    return button_policy_feed(&p, BUTTON_NONE, 240) == BUTTON_ACTION_TALK &&
           button_policy_feed(&p, BUTTON_NONE, 400) == BUTTON_ACTION_NONE;
}

constexpr bool LongAction(button_id_t key, uint32_t hold, button_action_t action)
{
    auto p = Released();
    button_policy_feed(&p, key, 100);
    button_policy_feed(&p, key, 140);
    if (button_policy_feed(&p, key, 140 + hold - 1) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, key, 140 + hold) != action) return false;
    if (button_policy_feed(&p, key, 140 + hold + 1000) != BUTTON_ACTION_NONE) return false;
    button_policy_feed(&p, BUTTON_NONE, 140 + hold + 1100);
    return button_policy_feed(&p, BUTTON_NONE, 140 + hold + 1140) == BUTTON_ACTION_NONE;
}

constexpr bool VolumeRepeat(uint32_t t = 0)
{
    auto p = Released(t);
    if (button_policy_feed(&p, BUTTON_VOL_UP, t + 100) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_VOL_UP, t + 140) != BUTTON_ACTION_VOL_UP) return false;
    if (button_policy_feed(&p, BUTTON_VOL_UP, t + 739) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_VOL_UP, t + 740) != BUTTON_ACTION_VOL_UP) return false;
    if (button_policy_feed(&p, BUTTON_VOL_UP, t + 889) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_VOL_UP, t + 890) != BUTTON_ACTION_VOL_UP) return false;
    button_policy_feed(&p, BUTTON_NONE, t + 900);
    return button_policy_feed(&p, BUTTON_NONE, t + 940) == BUTTON_ACTION_NONE;
}

constexpr bool BlockUntilRelease(button_id_t second)
{
    auto p = Released();
    button_policy_feed(&p, BUTTON_REC, 100);
    button_policy_feed(&p, BUTTON_REC, 140);
    button_policy_feed(&p, second, 200);
    if (button_policy_feed(&p, second, 240) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, second, 5000) != BUTTON_ACTION_NONE) return false;
    button_policy_feed(&p, BUTTON_NONE, 5100);
    if (button_policy_feed(&p, BUTTON_NONE, 5140) != BUTTON_ACTION_NONE) return false;
    button_policy_feed(&p, BUTTON_VOL_DOWN, 5200);
    return button_policy_feed(&p, BUTTON_VOL_DOWN, 5240) == BUTTON_ACTION_VOL_DOWN;
}

constexpr bool BootHeld()
{
    auto p = button_policy_initial();
    button_policy_feed(&p, BUTTON_REC, 0);
    if (button_policy_feed(&p, BUTTON_REC, 40) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, BUTTON_REC, 5000) != BUTTON_ACTION_NONE) return false;
    button_policy_feed(&p, BUTTON_NONE, 5010);
    return button_policy_feed(&p, BUTTON_NONE, 5050) == BUTTON_ACTION_NONE && !p.blocked;
}

constexpr bool Unassigned(button_id_t key)
{
    auto p = Released();
    button_policy_feed(&p, key, 100);
    if (button_policy_feed(&p, key, 140) != BUTTON_ACTION_NONE) return false;
    if (button_policy_feed(&p, key, 10000) != BUTTON_ACTION_NONE) return false;
    button_policy_feed(&p, BUTTON_NONE, 10010);
    return button_policy_feed(&p, BUTTON_NONE, 10050) == BUTTON_ACTION_NONE;
}

constexpr bool BounceDoesNotTrigger()
{
    auto p = Released();
    for (uint32_t t = 100; t < 300; t += 10) {
        if (button_policy_feed(&p, (t % 20) ? BUTTON_NONE : BUTTON_VOL_UP, t) !=
            BUTTON_ACTION_NONE) return false;
    }
    button_policy_feed(&p, BUTTON_VOL_UP, 300);
    return button_policy_feed(&p, BUTTON_VOL_UP, 339) == BUTTON_ACTION_NONE &&
           button_policy_feed(&p, BUTTON_VOL_UP, 340) == BUTTON_ACTION_VOL_UP;
}

constexpr bool ShortSetDoesNothing()
{
    auto p = Released();
    button_policy_feed(&p, BUTTON_SET, 100);
    button_policy_feed(&p, BUTTON_SET, 140);
    button_policy_feed(&p, BUTTON_NONE, 300);
    return button_policy_feed(&p, BUTTON_NONE, 340) == BUTTON_ACTION_NONE;
}

static_assert(button_from_mv(2310) == BUTTON_REC && button_from_mv(2510) == BUTTON_REC);
static_assert(button_from_mv(1880) == BUTTON_MUTE && button_from_mv(2080) == BUTTON_MUTE);
static_assert(button_from_mv(1550) == BUTTON_PLAY && button_from_mv(1750) == BUTTON_PLAY);
static_assert(button_from_mv(1010) == BUTTON_SET && button_from_mv(1210) == BUTTON_SET);
static_assert(button_from_mv(720) == BUTTON_VOL_DOWN && button_from_mv(920) == BUTTON_VOL_DOWN);
static_assert(button_from_mv(280) == BUTTON_VOL_UP && button_from_mv(480) == BUTTON_VOL_UP);
static_assert(button_from_mv(0) == BUTTON_INVALID && button_from_mv(2511) == BUTTON_INVALID);
static_assert(button_from_mv(2699) == BUTTON_INVALID && button_from_mv(2700) == BUTTON_NONE);
static_assert(ShortTalk());
static_assert(LongAction(BUTTON_REC, 2000, BUTTON_ACTION_END_TALK));
static_assert(LongAction(BUTTON_SET, 3000, BUTTON_ACTION_PROVISION));
static_assert(VolumeRepeat() && VolumeRepeat(0xfffffe00u));
static_assert(BlockUntilRelease(BUTTON_VOL_UP) && BlockUntilRelease(BUTTON_INVALID));
static_assert(BootHeld());
static_assert(Unassigned(BUTTON_MUTE) && Unassigned(BUTTON_PLAY));
static_assert(BounceDoesNotTrigger() && ShortSetDoesNothing());
static_assert(button_volume_next(0, false) == 0 && button_volume_next(100, true) == 100);
static_assert(button_volume_next(98, true) == 100 && button_volume_next(2, false) == 0);
static_assert(button_volume_next(40, true) == 45 && button_volume_next(40, false) == 35);
} // namespace
