/*
 * 编译期回归：向实际生产策略输入模拟的 AFE 帧，不依赖设备或 Windows 本机编译器。
 * xtensa-esp32s3-elf-g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/barge_in_guard_checks.cc
 * 只验证判定边界；真实人声/回声是否被 AFE 正确区分仍须在设备上测试。
 */
#include "../main/barge_in_guard.h"
#include "../main/audio_preroll_policy.h"

namespace {
constexpr uint32_t kFrameSamples = 512;  // 当前 AFE：16 kHz / 32 ms
constexpr int32_t kSpeechEnergy = 20000;

constexpr barge_in_result_t Feed(barge_in_guard_t &guard, bool speech = true,
                                 int32_t energy = kSpeechEnergy)
{
    return barge_in_guard_feed(&guard, speech, energy, kFrameSamples, 2000);
}

constexpr bool ContinuousSpeechStillNeeds480ms()
{
    barge_in_guard_t guard{};
    for (int frame = 0; frame < 14; ++frame) {
        if (Feed(guard, true, BARGE_IN_MIN_ENERGY) == BARGE_IN_CONFIRMED) return false;
    }
    return Feed(guard) == BARGE_IN_CONFIRMED &&
           barge_in_samples_to_ms(guard.samples) == 480 &&
           barge_in_samples_to_ms(guard.window_samples) == 480 &&
           guard.min_energy == BARGE_IN_MIN_ENERGY && guard.max_energy == kSpeechEnergy;
}

constexpr bool BriefGapsPreserveSpeech(bool weak_speech, int gap_frames)
{
    barge_in_guard_t guard{};
    for (int frame = 0; frame < 8; ++frame) {
        if (Feed(guard) == BARGE_IN_CONFIRMED) return false;
    }
    // 同时覆盖弱音和 VAD 短暂非人声（即使能量很高），它们都不能算有效人声。
    for (int frame = 0; frame < gap_frames; ++frame) {
        if (Feed(guard, weak_speech, weak_speech ? 9000 : 100000) != BARGE_IN_GAP_TOLERATED) {
            return false;
        }
    }
    if (barge_in_samples_to_ms(guard.samples) != 256) return false;
    for (int frame = 0; frame < 6; ++frame) {
        if (Feed(guard) == BARGE_IN_CONFIRMED) return false;
    }
    return Feed(guard) == BARGE_IN_CONFIRMED && guard.gap_samples == 0 &&
           guard.window_samples == (15u + gap_frames) * kFrameSamples &&
           guard.min_energy == kSpeechEnergy && guard.max_energy == kSpeechEnergy;
}

constexpr bool WindowBoundaryAllows160msTotalGaps()
{
    barge_in_guard_t guard{};
    // 160 ms 人声 + 96 ms 间隙 + 160 ms 人声 + 64 ms 间隙 + 160 ms 人声。
    for (int frame = 0; frame < 20; ++frame) {
        const bool speech = !(frame >= 5 && frame < 8) && !(frame >= 13 && frame < 15);
        const auto result = Feed(guard, speech, speech ? kSpeechEnergy : 0);
        if ((result == BARGE_IN_CONFIRMED) != (frame == 19)) return false;
    }
    return barge_in_samples_to_ms(guard.samples) == 480 &&
           barge_in_samples_to_ms(guard.window_samples) == 640;
}

constexpr bool LongPauseStartsNewCandidate()
{
    barge_in_guard_t guard{};
    for (int frame = 0; frame < 14; ++frame) Feed(guard);
    for (int frame = 0; frame < 3; ++frame) {
        if (Feed(guard, false, 0) != BARGE_IN_GAP_TOLERATED) return false;
    }
    if (Feed(guard, false, 0) != BARGE_IN_GAP_EXPIRED || guard.samples != 0 ||
        guard.window_samples != 0 || guard.gap_samples != 0) return false;
    for (int frame = 0; frame < 14; ++frame) {
        if (Feed(guard) == BARGE_IN_CONFIRMED) return false;
    }
    return Feed(guard) == BARGE_IN_CONFIRMED;
}

constexpr bool SparseBurstsCannotAccumulateForever()
{
    barge_in_guard_t guard{};
    int expired_windows = 0;
    // 每次间隙仅 64 ms，但十秒钟内的人声占比始终不足，不能拼成一次打断。
    for (int frame = 0; frame < 320; ++frame) {
        const bool speech = frame % 4 < 2;
        const auto result = Feed(guard, speech, speech ? kSpeechEnergy : 0);
        if (result == BARGE_IN_CONFIRMED) return false;
        if (result == BARGE_IN_WINDOW_EXPIRED) ++expired_windows;
    }
    return expired_windows > 0;
}

constexpr bool WeakOrNonSpeechFramesNeverConfirm()
{
    barge_in_guard_t weak{};
    barge_in_guard_t noise{};
    for (int frame = 0; frame < 320; ++frame) {
        if (Feed(weak, true, BARGE_IN_MIN_ENERGY - 1) != BARGE_IN_WAIT_ENERGY) return false;
        if (Feed(noise, false, 1000000) != BARGE_IN_WAIT_SPEECH) return false;
    }
    return weak.samples == 0 && noise.samples == 0;
}

constexpr bool StartupAndExplicitResetDiscardHistory()
{
    barge_in_guard_t guard{};
    for (int frame = 0; frame < 15; ++frame) Feed(guard);
    barge_in_guard_reset(&guard);  // 新一轮播放/恢复监听不能继承上次确认。
    if (guard.samples || guard.window_samples || guard.gap_samples ||
        guard.min_energy || guard.max_energy) return false;
    for (uint32_t age = 0; age < 1200; age += 32) {
        if (barge_in_guard_feed(&guard, true, kSpeechEnergy, kFrameSamples, age) !=
            BARGE_IN_WAIT_STARTUP) return false;
    }
    for (int frame = 0; frame < 14; ++frame) {
        if (barge_in_guard_feed(&guard, true, kSpeechEnergy, kFrameSamples, 1200) ==
            BARGE_IN_CONFIRMED) return false;
    }
    if (Feed(guard) != BARGE_IN_CONFIRMED) return false;
    // 新播放保护期也能清掉尚未显式 reset 的旧候选。
    return barge_in_guard_feed(&guard, true, kSpeechEnergy, kFrameSamples, 1199) ==
           BARGE_IN_WAIT_STARTUP && guard.samples == 0;
}

constexpr bool InvalidFramesBreakContinuity()
{
    barge_in_guard_t guard{};
    for (int frame = 0; frame < 14; ++frame) Feed(guard);
    if (barge_in_guard_feed(&guard, true, kSpeechEnergy, 0, 2000) !=
        BARGE_IN_INVALID_FRAME || guard.samples != 0) return false;
    Feed(guard);
    return barge_in_guard_feed(&guard, true, kSpeechEnergy, BARGE_IN_WINDOW_SAMPLES + 1, 2000) ==
           BARGE_IN_INVALID_FRAME && guard.samples == 0;
}

constexpr bool ConfirmationUsesSamplesNotFrameCount()
{
    barge_in_guard_t guard{};
    // 20 ms 帧应需 24 帧，而不能误用当前 32 ms 帧的 15 帧门限。
    for (int frame = 0; frame < 24; ++frame) {
        const auto result = barge_in_guard_feed(&guard, true, kSpeechEnergy, 320, 2000);
        if ((result == BARGE_IN_CONFIRMED) != (frame == 23)) return false;
    }
    return barge_in_samples_to_ms(guard.samples) == 480;
}

static_assert(ContinuousSpeechStillNeeds480ms(), "Strong speech must still meet the duration threshold");
static_assert(BriefGapsPreserveSpeech(true, 2), "A 64-ms weak syllable must preserve candidate speech");
static_assert(BriefGapsPreserveSpeech(true, 3), "A 96-ms weak syllable must preserve candidate speech");
static_assert(BriefGapsPreserveSpeech(false, 3), "A short VAD gap must not count as speech");
static_assert(WindowBoundaryAllows160msTotalGaps(), "480-ms speech within exactly 640 ms must confirm");
static_assert(LongPauseStartsNewCandidate(), "A 128-ms pause must discard previous speech");
static_assert(SparseBurstsCannotAccumulateForever(), "Sparse bursts must expire even with short gaps");
static_assert(WeakOrNonSpeechFramesNeverConfirm(), "Energy and VAD requirements must both remain active");
static_assert(StartupAndExplicitResetDiscardHistory(), "Startup protection and round resets must clear history");
static_assert(InvalidFramesBreakContinuity(), "Invalid input must discard pending confirmation");
static_assert(ConfirmationUsesSamplesNotFrameCount(), "Confirmation timing must use sample counts");
static_assert(BARGE_IN_SAMPLE_RATE == AUDIO_PREROLL_SAMPLE_RATE &&
              BARGE_IN_WINDOW_MS <= AUDIO_PREROLL_MS,
              "Preroll must cover the full barge-in candidate window");
}  // namespace
