/**
 * @file wake_word.c
 * @brief 唤醒词检测实现 — esp-sr WakeNet
 *
 * 任务分工：
 *  feed_Task   (core 0) : I²S DMA → AFE feed
 *  detect_Task (core 1) : AFE fetch → WAKENET_DETECTED → 回调
 *
 * PCM 数据通过 ring buffer 共享给 audio_in：
 *  detect_Task 写入 AFE 输出 → ring buffer → audio_in_task 编码上传
 */

#include "wake_word.h"
#include "board_config.h"
#include "event_bus.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "audio_out.h"
#include "app_state.h"
#include "model_path.h"
#include "string.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"
#include <stdatomic.h>
#include "barge_in_guard.h"
#include "audio_preroll_policy.h"
#include "esp_timer.h"

static const char *TAG = "wake_word";

_Static_assert(BARGE_IN_SAMPLE_RATE == BSP_I2S_SAMPLE_RATE,
               "Barge-in timing must match the AFE sample rate");

/* ===========================================================================
 * 全局状态
 * ========================================================================= */

/** AFE 接口与实例 */
static const esp_afe_sr_iface_t *s_afe_iface     = NULL;
static void                    *s_afe_data_wake = NULL;  /* Full AFE (with WakeNet) - for WAIT_WAKE */
static void                    *s_afe_data_lite = NULL;  /* AEC+VAD only (no WakeNet) - for LISTENING/PLAYING */
static void                    *s_afe_data      = NULL;  /* Currently active handle */
static wake_word_mode_t         s_mode          = WAKE_WORD_MODE_WAKE;

/** ring buffer 句柄（detect_Task → audio_in） */
static RingbufHandle_t  s_ringbuf = NULL;

/** feed/detect 任务句柄 */
static TaskHandle_t     s_feed_task_h    = NULL;
static TaskHandle_t     s_detect_task_h  = NULL;

/** 运行标志 */
static volatile bool    s_task_running   = false;

/** 唤醒状态（detect_Task 写，app_main 读） */
static volatile wake_word_state_t s_wake_state = WAKE_STATE_IDLE;

/** 唤醒回调 */
static wake_word_detected_cb_t  s_wake_cb       = NULL;
static void                    *s_wake_cb_arg   = NULL;

/** 栈高水位 */
static uint32_t s_feed_stack_hiwat   = 0;
static uint32_t s_detect_stack_hiwat = 0;

/** fetch 帧大小（samples） */
static int s_afe_chunksize = 0;
static int s_fetch_chunksize = 0;
static barge_in_guard_t s_barge_guard;
static uint32_t s_barge_reject_log_ms;
static bool s_barge_pending = false;
static int64_t s_playback_started_ms = 0;
static bool s_playback_seen = false;
static SemaphoreHandle_t s_audio_buffer_lock;
static atomic_bool s_barge_reset_requested = false;

/* 以下缓存状态由 s_audio_buffer_lock 保护；检测锁存与缓存保护分别管理。
 * 恢复监听会清除检测锁存，但必须等补传成功后才能解除音频保护。
 */
static bool s_preroll_protected = false;
static uint32_t s_preroll_id = 0;
static size_t s_preroll_dropped_samples = 0;

static size_t audio_buffer_frames_locked(void)
{
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_ringbuf, NULL, NULL, NULL, NULL, &waiting);
    return waiting;
}

static bool discard_oldest_audio_locked(void)
{
    size_t bytes;
    void *old = xRingbufferReceive(s_ringbuf, &bytes, 0);
    if (!old) return false;
    vRingbufferReturnItem(s_ringbuf, old);
    return true;
}

/* 清空和模式切换共用此入口，防止恢复新会话时继承旧的缓存保护。调用方持锁。 */
static void clear_audio_buffer_locked(void)
{
    while (discard_oldest_audio_locked()) {}
    s_preroll_protected = false;
    s_preroll_dropped_samples = 0;
}

/* 只在检测任务调用。当前 AFE 每项固定为一个 fetch 帧，队列项数就是 PCM 帧数。
 * 返回非零批次号时，历史音频及当前确认帧已经受保护，可以发布打断事件。
 */
static uint32_t buffer_afe_audio(const afe_fetch_result_t *res, bool confirm_barge_in)
{
    if (!s_ringbuf || !res->data || res->data_size <= 0) return 0;
    const size_t frame_bytes = (size_t)s_fetch_chunksize * sizeof(int16_t);
    if ((size_t)res->data_size != frame_bytes) {
        ESP_LOGW(TAG, "Unexpected AFE frame size: %d (expected %u)",
                 res->data_size, (unsigned)frame_bytes);
        return 0;
    }

    uint32_t protected_id = 0;
    size_t protected_frames = 0;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    /* 在锁内重新读取状态，避免已恢复监听却按旧 PLAYING 状态裁剪缓存。 */
    app_state_t current_state = app_state_get();
    if (current_state != APP_STATE_LISTENING && current_state != APP_STATE_PLAYING &&
        current_state != APP_STATE_CANCELLING) {
        xSemaphoreGive(s_audio_buffer_lock);
        return 0;
    }

    size_t discard = audio_preroll_frames_to_drop(audio_buffer_frames_locked(),
        s_fetch_chunksize, current_state == APP_STATE_PLAYING, s_preroll_protected);
    while (discard > 0 && discard_oldest_audio_locked()) --discard;

    if (confirm_barge_in && current_state == APP_STATE_PLAYING && !s_preroll_protected) {
        s_preroll_protected = true;
        if (++s_preroll_id == 0) ++s_preroll_id;
        s_preroll_dropped_samples = 0;
        protected_id = s_preroll_id;
    }

    /* 正常监听超载时保留较新的音频；保护期间缓存满时丢新帧并报告，绝不静默删句首。
     * 现有约 10 秒容量覆盖 1.5 秒预录音 + 最长 8 秒取消等待，无需再分配大缓存。
     */
    while (xRingbufferSend(s_ringbuf, res->data, frame_bytes, 0) != pdTRUE) {
        if (s_preroll_protected) {
            if (s_preroll_dropped_samples == 0) {
                ESP_LOGW(TAG, "Barge-in buffer full: preserving prefix, dropping new audio (id=%lu)",
                         (unsigned long)s_preroll_id);
            }
            s_preroll_dropped_samples += s_fetch_chunksize;
            break;
        }
        if (!discard_oldest_audio_locked()) break;
    }
    if (protected_id) protected_frames = audio_buffer_frames_locked();
    xSemaphoreGive(s_audio_buffer_lock);

    if (protected_id) {
        ESP_LOGI(TAG, "Barge-in pre-roll protected: id=%lu frames=%u audio_ms=%u",
                 (unsigned long)protected_id, (unsigned)protected_frames,
                 (unsigned)audio_preroll_samples_to_ms(protected_frames * s_fetch_chunksize));
    }
    return protected_id;
}

static int32_t afe_frame_energy(const int16_t *pcm, int samples)
{
    if (!pcm || samples <= 0) return 0;
    int64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        int32_t sample = pcm[i];
        sum += (int64_t)sample * sample;
    }
    return (int32_t)(sum / samples);
}

/* ===========================================================================
 * feed_Task — I²S DMA → AFE
 * ========================================================================= */

static void feed_Task(void *arg)
{
    (void)arg;
    int16_t *mic_buff = NULL;
    int16_t *ref_buff = NULL;
    int16_t *afe_buff = NULL;

    /* AFE 输入为 MMR：两路麦克风 + 一路数字播放参考。 */
    const int mic_channels = 2;
    const int afe_channels = 3;
    size_t mic_size = s_afe_chunksize * mic_channels * sizeof(int16_t);
    mic_buff = malloc(mic_size);
    ref_buff = malloc(s_afe_chunksize * sizeof(int16_t));
    afe_buff = malloc(s_afe_chunksize * afe_channels * sizeof(int16_t));
    if (mic_buff == NULL || ref_buff == NULL || afe_buff == NULL) {
        ESP_LOGE(TAG, "feed: audio buffer allocation failed");
        free(mic_buff);
        free(ref_buff);
        free(afe_buff);
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed_Task started (chunksize=%d, input=MMR, core %d)",
             s_afe_chunksize, xPortGetCoreID());
    int64_t stats_at = esp_timer_get_time();
    uint32_t feed_max_us = 0, feed_overruns = 0, feed_count = 0;

    while (s_task_running) {
        /* 从 I²S DMA 读取 PCM */
        int ret = audio_out_read_microphones(mic_buff, mic_size);
        if (ret == 0) {
            audio_out_read_playback_reference(ref_buff, s_afe_chunksize);
            for (int i = 0; i < s_afe_chunksize; ++i) {
                afe_buff[3 * i]     = mic_buff[2 * i];
                afe_buff[3 * i + 1] = mic_buff[2 * i + 1];
                afe_buff[3 * i + 2] = ref_buff[i];
            }
            /* 喂给 AFE，最后一路必须是播放 reference。 */
            int64_t start = esp_timer_get_time();
            s_afe_iface->feed(s_afe_data, afe_buff);
            uint32_t elapsed = (uint32_t)(esp_timer_get_time() - start);
            if (elapsed > feed_max_us) feed_max_us = elapsed;
            if (elapsed > (uint32_t)s_afe_chunksize * 1000000u / 16000u) ++feed_overruns;
            ++feed_count;
            if (esp_timer_get_time() - stats_at >= 5000000) {
                ESP_LOGI(TAG, "AFE stats: frames=%lu feed_max_us=%lu over_budget=%lu budget_us=%u",
                         (unsigned long)feed_count, (unsigned long)feed_max_us,
                         (unsigned long)feed_overruns, (unsigned)s_afe_chunksize * 1000000u / 16000u);
                stats_at = esp_timer_get_time();
                feed_count = feed_max_us = feed_overruns = 0;
            }
            /* A backlogged RX queue may never block. Yield one tick per 64 ms
             * input frame so CPU0 idle/system work is not starved indefinitely. */
            vTaskDelay(1);
        } else {
            /* 数据未就绪，短延时重试 */
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    free(mic_buff);
    free(ref_buff);
    free(afe_buff);
    ESP_LOGI(TAG, "feed_Task exited");
    vTaskDelete(NULL);
}

/* ===========================================================================
 * detect_Task — AFE fetch → 唤醒词检测
 * ========================================================================= */

static const char *barge_in_result_name(barge_in_result_t result)
{
    switch (result) {
    case BARGE_IN_WAIT_STARTUP:   return "startup_guard";
    case BARGE_IN_INVALID_FRAME:  return "invalid_frame";
    case BARGE_IN_WAIT_SPEECH:    return "not_speech";
    case BARGE_IN_WAIT_ENERGY:    return "low_energy";
    case BARGE_IN_COLLECTING:     return "collecting";
    case BARGE_IN_GAP_TOLERATED:  return "short_gap";
    case BARGE_IN_GAP_EXPIRED:    return "gap_expired";
    case BARGE_IN_WINDOW_EXPIRED: return "window_expired";
    case BARGE_IN_CONFIRMED:      return "confirmed";
    default:                     return "unknown";
    }
}

static void detect_Task(void *arg)
{
    (void)arg;

    /* fetch 输出缓冲区（AFE 可能输出多通道，回合单通道给 audio_in） */
    int16_t *fetch_buff = malloc(s_afe_chunksize * sizeof(int16_t));
    if (fetch_buff == NULL) {
        ESP_LOGE(TAG, "detect: malloc failed");
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "detect_Task started (core %d)", xPortGetCoreID());
    ESP_LOGI(TAG, "Playback barge-in guard: energy>=%d, speech=%u ms, max_gap=%u ms, "
             "window=%u ms, startup_guard=%u ms", BARGE_IN_MIN_ENERGY,
             BARGE_IN_CONFIRM_MS, BARGE_IN_MAX_GAP_MS, BARGE_IN_WINDOW_MS, BARGE_IN_GUARD_MS);

    while (s_task_running) {
        /* 从 AFE 取回处理结果 */
        afe_fetch_result_t *res = s_afe_iface->fetch(s_afe_data);
        bool confirm_barge_in = false;

        if (res == NULL || res->ret_value == ESP_FAIL) {
            barge_in_guard_reset(&s_barge_guard);
            ESP_LOGW(TAG, "AFE fetch failed or EOF");
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (atomic_exchange(&s_barge_reset_requested, false)) {
            barge_in_guard_reset(&s_barge_guard);
            s_barge_pending = false;
        }

        /* 仅确认后请求取消。短暂弱音保留候选进度，但不计入有效人声时长；
         * 未确认前不静音、不暂停、不丢弃播放数据。
         */
        if (audio_out_is_streaming() && app_state_get() == APP_STATE_PLAYING) {
            if (!s_playback_seen) {
                s_playback_seen = true;
                s_playback_started_ms = esp_log_timestamp();
            }
            const uint32_t now = esp_log_timestamp();
            const uint32_t playback_age = now - (uint32_t)s_playback_started_ms;
            const int samples = res->data && res->data_size > 0
                                ? res->data_size / sizeof(int16_t) : 0;
            const int32_t processed_energy = afe_frame_energy(res->data, samples);
            if (!s_barge_pending) {
                const bool speech = res->vad_state == VAD_SPEECH;
                const barge_in_guard_t before = s_barge_guard;
                const barge_in_result_t result = barge_in_guard_feed(&s_barge_guard,
                    speech, processed_energy, samples, playback_age);
                if (result == BARGE_IN_CONFIRMED) {
                    confirm_barge_in = true;
                    ESP_LOGI(TAG, "Barge-in confirmed: energy=%ld min=%ld max=%ld "
                             "speech_ms=%u window_ms=%u gaps_ms=%u",
                             (long)processed_energy, (long)s_barge_guard.min_energy,
                             (long)s_barge_guard.max_energy,
                             (unsigned)barge_in_samples_to_ms(s_barge_guard.samples),
                             (unsigned)barge_in_samples_to_ms(s_barge_guard.window_samples),
                             (unsigned)barge_in_samples_to_ms(
                                 s_barge_guard.window_samples - s_barge_guard.samples));
                } else if (now - s_barge_reject_log_ms >= 2000 &&
                           (before.samples || s_barge_guard.samples ||
                            (speech && processed_energy >= BARGE_IN_DIAGNOSTIC_ENERGY))) {
                    /* 最多每 2 秒记录一次候选情况，避免逐帧日志影响音频处理。
                     * 前后计数可区分仍在累计、短隙保留和超限清零；不能据此认定是人声还是回声。
                     */
                    s_barge_reject_log_ms = now;
                    ESP_LOGI(TAG, "Barge-in pending: reason=%s vad=%d energy=%ld "
                             "speech_ms=%u->%u window_ms=%u->%u gap_ms=%u->%u",
                             barge_in_result_name(result), (int)speech, (long)processed_energy,
                             (unsigned)barge_in_samples_to_ms(before.samples),
                             (unsigned)barge_in_samples_to_ms(s_barge_guard.samples),
                             (unsigned)barge_in_samples_to_ms(before.window_samples),
                             (unsigned)barge_in_samples_to_ms(s_barge_guard.window_samples),
                             (unsigned)barge_in_samples_to_ms(before.gap_samples),
                             (unsigned)barge_in_samples_to_ms(s_barge_guard.gap_samples));
                }
            }
        } else {
            barge_in_guard_reset(&s_barge_guard);
            s_playback_seen = false;
            s_playback_started_ms = 0;
            /* Keep the latch until the cancelled response is acknowledged.
             * Old binary frames can arrive after audio_out_stop(); clearing
             * it here would retrigger barge-in for the same utterance. */
        }

        /* VAD state: 0=silence, 1=speech (via AFE 内置 VAD) */
        // ESP_LOGD(TAG, "vad=%d wake=%d", res->vad_state, res->wakeup_state);

        /* ── 唤醒词检测 ── */
        /* B2: only run WakeNet during WAIT_WAKE (CPU saving during LISTENING/PLAYING) */
        /* In LISTENING/PLAYING conversation_active=true,so any hit is discarded by app_main. */
        /* feed_task / AEC / VAD / barge-in keep running,audio_in and echo cancellation unaffected. */
        if (app_state_get() == APP_STATE_WAIT_WAKE &&
            res->wakeup_state == WAKENET_DETECTED) {
            if (s_wake_state == WAKE_STATE_IDLE) {
                s_wake_state = WAKE_STATE_DETECTED;
                ESP_LOGI(TAG, ">>> WakeWord DETECTED! model=%d word=%d <<<",
                         res->wakenet_model_index, res->wake_word_index);

                /* 回调通知 app_main */
                if (s_wake_cb) {
                    s_wake_cb(res->wake_word_index, res->wakenet_model_index, s_wake_cb_arg);
                }

                /* 发布事件总线消息（与 EVENT_BUS_* 兼容宏对齐） */
                event_bus_publish(EV_AUDIO_WAKE_DETECTED, NULL, 0);
            }
            /* 检测到后不再重复触发，直到 reset() */
        }

        /* 先保护历史并追加确认帧，再发布事件。主任务等播放锁或取消确认时仍持续录音。 */
        uint32_t protected_id = buffer_afe_audio(res, confirm_barge_in);
        if (protected_id) {
            s_barge_pending = true;
            if (event_bus_publish(EV_AUDIO_BARGE_IN, NULL, 0) != pdTRUE) {
                s_barge_pending = false;
                wake_word_finish_preroll(protected_id);
                ESP_LOGW(TAG, "Barge-in event queue full; retrying detection");
            }
        }
    }

    free(fetch_buff);
    ESP_LOGI(TAG, "detect_Task exited");
    vTaskDelete(NULL);
}

/* ===========================================================================
 * 公共 API
 * ========================================================================= */

esp_err_t wake_word_init(void)
{
    if (s_task_running) {
        ESP_LOGW(TAG, "already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WakeWord (esp-sr)...");

    /* ── 1. 板级初始化（I²S 总线、ES7210）────────────────── */
    esp_err_t err = audio_out_init(ES8311_I2C_ADDR);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_board_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ── 2. 加载模型文件（从 flash partition "model"）─────── */
    srmodel_list_t *models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "esp_srmodel_init failed — no model partition? "
                      "run `idf.py menuconfig → ESP Speech Recognition → Select wake words`");
        return ESP_FAIL;
    }
    for (int i = 0; i < models->num; i++) {
        if (strstr(models->model_name[i], ESP_WN_PREFIX) != NULL) {
            ESP_LOGI(TAG, "WakeNet model found: %s", models->model_name[i]);
        }
    }

    /* ── 3. AFE 配置 ─────────────────────────────────────── */
    /* === 3. AFE configuration: create two configs (WAKE and LITE) ===
     * WAKE config keeps the auto-loaded WakeNet model.
     * LITE config sets wakenet_model_name = NULL to skip WakeNet allocation
     * (CPU savings in LISTENING/PLAYING). Both share the same interface. */
    afe_config_t *cfg_wake = afe_config_init("MMR", models,
                                              AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (cfg_wake == NULL) {
        ESP_LOGE(TAG, "afe_config_init (WAKE) failed");
        return ESP_FAIL;
    }

    /* Full-duplex AEC: the MMR last channel is the digital playback reference. */
    cfg_wake->aec_init = true;
    cfg_wake->aec_mode = AEC_MODE_FD_LOW_COST;
    cfg_wake->aec_filter_length = 4;
    cfg_wake->output_playback_channel = false;
    ESP_LOGI(TAG, "AFE configured for full-duplex AEC (MMR, filter=%d)",
             cfg_wake->aec_filter_length);

    /* 可选：打印/覆盖唤醒词模型名称 */
    if (cfg_wake->wakenet_model_name) {
        ESP_LOGI(TAG, "WakeNet model: %s", cfg_wake->wakenet_model_name);
    }
    if (cfg_wake->wakenet_model_name_2) {
        ESP_LOGI(TAG, "WakeNet model 2: %s", cfg_wake->wakenet_model_name_2);
    }

    /* Clone for LITE; disable WakeNet by clearing model names. */
    afe_config_t *cfg_lite = afe_config_init("MMR", models,
                                              AFE_TYPE_FD, AFE_MODE_LOW_COST);
    if (cfg_lite == NULL) {
        ESP_LOGE(TAG, "afe_config_init (LITE) failed");
        afe_config_free(cfg_wake);
        return ESP_FAIL;
    }
    cfg_lite->aec_init = true;
    cfg_lite->aec_mode = AEC_MODE_FD_LOW_COST;
    cfg_lite->aec_filter_length = 4;
    cfg_lite->output_playback_channel = false;
    /* KEY: skip WakeNet allocation in LITE mode */
    cfg_lite->wakenet_model_name   = NULL;
    cfg_lite->wakenet_model_name_2 = NULL;

    /* === 4. Get interface and create both handles === */
    s_afe_iface = esp_afe_handle_from_config(cfg_wake);
    if (s_afe_iface == NULL) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        afe_config_free(cfg_wake);
        afe_config_free(cfg_lite);
        return ESP_FAIL;
    }

    size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM before AFE handles: free=%u KB",
             (unsigned)(psram_before / 1024));

    s_afe_data_wake = s_afe_iface->create_from_config(cfg_wake);
    if (s_afe_data_wake == NULL) {
        ESP_LOGE(TAG, "AFE WAKE handle create failed");
        afe_config_free(cfg_wake);
        afe_config_free(cfg_lite);
        return ESP_FAIL;
    }
    size_t psram_after_wake = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM after WAKE handle: free=%u KB (delta=%u KB)",
             (unsigned)(psram_after_wake / 1024),
             (unsigned)((psram_before - psram_after_wake) / 1024));

    s_afe_data_lite = s_afe_iface->create_from_config(cfg_lite);
    if (s_afe_data_lite == NULL) {
        ESP_LOGE(TAG, "AFE LITE handle create failed");
        s_afe_iface->destroy(s_afe_data_wake);
        s_afe_data_wake = NULL;
        afe_config_free(cfg_wake);
        afe_config_free(cfg_lite);
        return ESP_FAIL;
    }
    size_t psram_after_lite = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM after LITE handle: free=%u KB (delta=%u KB)",
             (unsigned)(psram_after_lite / 1024),
             (unsigned)((psram_after_wake - psram_after_lite) / 1024));

    afe_config_free(cfg_wake);
    afe_config_free(cfg_lite);

    /* === 5. Get chunksize; verify WAKE and LITE match (warning only) === */
    int cs_wake = s_afe_iface->get_feed_chunksize(s_afe_data_wake);
    int cs_lite = s_afe_iface->get_feed_chunksize(s_afe_data_lite);
    if (cs_wake != cs_lite) {
        ESP_LOGE(TAG, "chunksize mismatch: wake=%d lite=%d - using larger, "
                      "smaller handle may misbehave", cs_wake, cs_lite);
    }
    s_afe_chunksize   = (cs_wake > cs_lite) ? cs_wake : cs_lite;
    /* POC-1 fix: s_fetch_chunksize was missing - broke audio upload */
    s_fetch_chunksize  = s_afe_iface->get_fetch_chunksize(s_afe_data_wake);
    ESP_LOGI(TAG, "AFE chunksize=%d samples (%.1f ms) [wake=%d lite=%d fetch=%d]",
             s_afe_chunksize, s_afe_chunksize * 1000.0f / 16000.0f,
             cs_wake, cs_lite, s_fetch_chunksize);

    /* ── 6. 创建 ring buffer ─────────────────────────────── */
    s_ringbuf = xRingbufferCreateWithCaps(WAKE_WORD_RB_TOTAL_SIZE, RINGBUF_TYPE_NOSPLIT,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_audio_buffer_lock = xSemaphoreCreateMutex();
    if (s_ringbuf == NULL || s_audio_buffer_lock == NULL) {
        ESP_LOGE(TAG, "xRingbufferCreate failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Ring buffer: %d items × %d samples = %d bytes",
             WAKE_WORD_RB_ITEM_COUNT, WAKE_WORD_RB_ITEM_SIZE,
             (int)WAKE_WORD_RB_TOTAL_SIZE);

    /* ── 7. 启动 feed_Task + detect_Task ────────────────── */
    /* Set active handle BEFORE starting tasks (CRITICAL: otherwise tasks see NULL handle) */
    s_afe_data = s_afe_data_wake;
    s_mode = WAKE_WORD_MODE_WAKE;

    s_task_running = true;
    BaseType_t t0 = xTaskCreatePinnedToCore(
        feed_Task, "ww_feed",
        WAKE_WORD_FEED_STACK, NULL, 5, &s_feed_task_h, 0);
    BaseType_t t1 = xTaskCreatePinnedToCore(
        detect_Task, "ww_detect",
        WAKE_WORD_DETECT_STACK, NULL, 5, &s_detect_task_h, 1);

    if (t0 != pdPASS || t1 != pdPASS) {
        ESP_LOGE(TAG, "task creation failed (feed=%d detect=%d)", t0, t1);
        s_task_running = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WakeWord initialized — feed (core0) + detect (core1) running");
    return ESP_OK;
}

void wake_word_deinit(void)
{
    if (!s_task_running) return;

    ESP_LOGI(TAG, "Stopping WakeWord...");
    s_task_running = false;

    /* 等待任务退出 */
    if (s_feed_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_feed_task_h = NULL;
    }
    if (s_detect_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_detect_task_h = NULL;
    }

    /* 释放 AFE */
    /* Release AFE handles (destroy LITE first, then WAKE) */
    if (s_afe_iface) {
        if (s_afe_data_lite) {
            s_afe_iface->destroy(s_afe_data_lite);
            s_afe_data_lite = NULL;
        }
        if (s_afe_data_wake) {
            s_afe_iface->destroy(s_afe_data_wake);
            s_afe_data_wake = NULL;
        }
        s_afe_data  = NULL;
        s_afe_iface = NULL;
    }

    /* 释放 ring buffer */
    if (s_ringbuf) {
        vRingbufferDeleteWithCaps(s_ringbuf);
        s_ringbuf = NULL;
    }

    s_wake_state = WAKE_STATE_IDLE;
    ESP_LOGI(TAG, "WakeWord deinitialized");
}

RingbufHandle_t wake_word_get_ringbuf(void)
{
    return s_ringbuf;
}

bool wake_word_get_preroll_info(wake_word_preroll_info_t *info)
{
    if (!info) return false;
    memset(info, 0, sizeof(*info));
    if (!s_ringbuf || !s_audio_buffer_lock) return false;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    bool available = s_preroll_protected;
    if (available) {
        info->id = s_preroll_id;
        info->frames = audio_buffer_frames_locked();
        info->samples = info->frames * s_fetch_chunksize;
    }
    xSemaphoreGive(s_audio_buffer_lock);
    return available;
}

size_t wake_word_finish_preroll(uint32_t id)
{
    if (!s_audio_buffer_lock || !id) return 0;
    size_t dropped = 0;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    if (id == s_preroll_id && s_preroll_protected) {
        dropped = s_preroll_dropped_samples;
        s_preroll_protected = false;
        s_preroll_dropped_samples = 0;
    }
    xSemaphoreGive(s_audio_buffer_lock);
    return dropped;
}

int wake_word_get_chunksize(void)
{
    return s_fetch_chunksize > 0 ? s_fetch_chunksize : 512;
}

wake_word_state_t wake_word_get_state(void)
{
    return s_wake_state;
}

void wake_word_set_callback(wake_word_detected_cb_t cb, void *user_data)
{
    s_wake_cb     = cb;
    s_wake_cb_arg = user_data;
}

void wake_word_reset(void)
{
    if (s_wake_state != WAKE_STATE_IDLE) {
        ESP_LOGI(TAG, "wake_word_reset: DETECTED → IDLE");
        s_wake_state = WAKE_STATE_IDLE;
    }
}

void wake_word_reset_barge_in(void)
{
    atomic_store(&s_barge_reset_requested, true);
}

void wake_word_clear_audio_buffer(void)
{
    if (!s_ringbuf || !s_audio_buffer_lock) return;
    xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    clear_audio_buffer_locked();
    xSemaphoreGive(s_audio_buffer_lock);
}

esp_err_t wake_word_start(void)
{
    if (s_task_running) {
        ESP_LOGD(TAG, "wake_word already running");
        return ESP_OK;
    }
    /* 尚未初始化 → 完整初始化（首次调用） */
    if (s_afe_iface == NULL) {
        return wake_word_init();
    }
    /* 已初始化但任务停止 → 重启任务 */
    s_task_running = true;
    BaseType_t t0 = xTaskCreatePinnedToCore(
        feed_Task, "ww_feed",
        WAKE_WORD_FEED_STACK, NULL, 5, &s_feed_task_h, 0);
    BaseType_t t1 = xTaskCreatePinnedToCore(
        detect_Task, "ww_detect",
        WAKE_WORD_DETECT_STACK, NULL, 5, &s_detect_task_h, 1);
    if (t0 != pdPASS || t1 != pdPASS) {
        ESP_LOGE(TAG, "task restart failed (feed=%d detect=%d)", t0, t1);
        s_task_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "WakeWord tasks restarted (AFE preserved)");
    return ESP_OK;
}

void wake_word_stop(void)
{
    if (!s_task_running) {
        ESP_LOGD(TAG, "wake_word already stopped");
        return;
    }
    ESP_LOGI(TAG, "Stopping WakeWord tasks (preserving AFE)...");
    s_task_running = false;

    /* 等待任务退出 */
    if (s_feed_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_feed_task_h = NULL;
    }
    if (s_detect_task_h) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_detect_task_h = NULL;
    }

    s_wake_state = WAKE_STATE_IDLE;
    ESP_LOGI(TAG, "WakeWord tasks stopped (AFE handle preserved for restart)");
}

uint32_t wake_word_get_feed_stack_hiwat(void)
{
    return s_feed_stack_hiwat;
}

uint32_t wake_word_get_detect_stack_hiwat(void)
{
    return s_detect_stack_hiwat;
}

esp_err_t wake_word_set_mode(wake_word_mode_t mode)
{
    void *target = (mode == WAKE_WORD_MODE_WAKE) ? s_afe_data_wake : s_afe_data_lite;
    if (target == NULL) {
        ESP_LOGE(TAG, "set_mode: AFE handles not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_afe_data == target) {
        /* Already in target mode - fast path, no suspend needed */
        return ESP_OK;
    }
    if (s_feed_task_h == NULL || s_detect_task_h == NULL) {
        ESP_LOGE(TAG, "set_mode: tasks not running");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "AFE mode switch: %s -> %s",
             s_mode == WAKE_WORD_MODE_WAKE ? "WAKE" : "LITE",
             mode == WAKE_WORD_MODE_WAKE ? "WAKE" : "LITE");

    /* 先取缓存锁再挂起检测任务，避免把持锁的任务挂起后永远等不到锁。 */
    if (s_audio_buffer_lock) xSemaphoreTake(s_audio_buffer_lock, portMAX_DELAY);
    vTaskSuspend(s_feed_task_h);
    vTaskSuspend(s_detect_task_h);

    s_afe_data = target;
    s_mode = mode;

    /* Drain stale PCM (old AFE output invalid for new handle) */
    clear_audio_buffer_locked();

    /* Reset wake detection state to prevent spurious triggers from cache */
    s_wake_state = WAKE_STATE_IDLE;

    if (s_audio_buffer_lock) xSemaphoreGive(s_audio_buffer_lock);
    vTaskResume(s_detect_task_h);
    vTaskResume(s_feed_task_h);

    return ESP_OK;
}

wake_word_mode_t wake_word_get_mode(void)
{
    return s_mode;
}
