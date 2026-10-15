/*
 * 不依赖板子的编译期回归：用连续帧编号表示输入音频，检查真实保留策略是否丢句首。
 * 在 ESP-IDF 终端执行：
 * xtensa-esp32s3-elf-g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/audio_preroll_checks.cc
 * static_assert 在交叉编译时求值，无需在 Windows 运行 ESP32 可执行文件。
 */
#include "../main/audio_preroll_policy.h"

#include <array>

namespace {
struct CaptureWindow {
    std::array<int, 320> frames{};
    size_t head = 0;
    size_t count = 0;
    size_t dropped_new = 0;

    constexpr int Pop()
    {
        if (!count) return -1;
        const int value = frames[head];
        head = (head + 1) % frames.size();
        --count;
        return value;
    }

    constexpr void Push(int value, bool playing, bool protected_audio)
    {
        size_t discard = audio_preroll_frames_to_drop(count, 512, playing, protected_audio);
        while (discard > 0) {
            Pop();
            --discard;
        }
        if (count == frames.size()) {
            if (protected_audio) {
                ++dropped_new;
                return;
            }
            Pop();
        }
        frames[(head + count) % frames.size()] = value;
        ++count;
    }
};

constexpr bool LongPlaybackKeepsRecentSpeech()
{
    CaptureWindow window;
    for (int frame = 0; frame < 2000; ++frame) window.Push(frame, true, false);
    if (window.count != 47) return false;
    // 64 秒播放后仍保存最近 1504 ms，而不是最早的回声或仅最后 32 ms。
    for (int frame = 1953; frame < 2000; ++frame) {
        if (window.Pop() != frame) return false;
    }
    return window.count == 0;
}

constexpr bool DelayedCancellationPreservesPrefix()
{
    CaptureWindow window;
    for (int frame = 0; frame < 200; ++frame) window.Push(frame, true, false);
    // 讲话从第 185 帧开始，200 帧确认打断。主任务尚未切出 PLAYING。
    window.Push(200, true, false);
    for (int frame = 201; frame < 214; ++frame) window.Push(frame, true, true);
    // 继续等待最多 8 秒取消确认；全部帧接在保护的句首之后。
    for (int frame = 214; frame < 464; ++frame) window.Push(frame, false, true);
    if (window.count != 310 || window.dropped_new != 0) return false;
    // 上传与新音频交错时，必须按原始次序衔接，不能重复或跳过帧。
    for (int frame = 154; frame < 464; ++frame) {
        if (window.Pop() != frame) return false;
        if (frame < 164) window.Push(464 + frame - 154, false, true);
    }
    for (int frame = 464; frame < 474; ++frame) {
        if (window.Pop() != frame) return false;
    }
    return window.count == 0;
}

constexpr bool ListeningDoesNotTrimToPreroll()
{
    CaptureWindow window;
    for (int frame = 0; frame < 100; ++frame) window.Push(frame, false, false);
    if (window.count != 100) return false;
    for (int frame = 0; frame < 100; ++frame) {
        if (window.Pop() != frame) return false;
    }
    return true;
}

constexpr bool OverflowPreservesProtectedBeginning()
{
    CaptureWindow window;
    for (int frame = 0; frame < 340; ++frame) window.Push(frame, false, true);
    if (window.count != 320 || window.dropped_new != 20) return false;
    for (int frame = 0; frame < 320; ++frame) {
        if (window.Pop() != frame) return false;
    }
    return true;
}

static_assert(LongPlaybackKeepsRecentSpeech(), "Playback must keep recent 1.5-second history");
static_assert(DelayedCancellationPreservesPrefix(), "Cancellation must retain speech onset and FIFO order");
static_assert(ListeningDoesNotTrimToPreroll(), "Normal upload backlog must not use playback trimming");
static_assert(OverflowPreservesProtectedBeginning(), "Protected history must survive capacity exhaustion");
}  // namespace
