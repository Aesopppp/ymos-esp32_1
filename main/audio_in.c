/**
 * @file audio_in.c
 * @brief 音频采集服务 (P0 收音链路核心)
 *
 * 架构说明（唤醒词模式）：
 *  wake_word.c 的 feed_Task 持续读取 I²S DMA，detect_Task 将 AFE 输出写入 ring buffer。
 *  audio_in_start_from_ringbuf() 启动后从该 ring buffer 读取 PCM，
 *  自身不再初始化 I²S。
 *
 *  链路: (AFE) → ring buffer → VAD → PCM 缓冲 → Opus 编码 → WS JSON
 *
 *  采样: 16 kHz, 16-bit, mono
 *  VAD:  能量阈值 + 静音超时（由 wake_word 通过 AFE 提供统一 PCM 流）
 */

#include "audio_in.h"
#include "board_config.h"
#include "wake_word.h"
#include "ws_service.h"
#include "opus_codec.h"
#include "audio_preroll_policy.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <stdatomic.h>

static const char *TAG = "audio_in";

/** 全局句柄（供 app_state.c 启动时使用） */
static audio_in_handle_t s_audio_handle = NULL;

/* ===========================================================================
 * 内部结构
 * ========================================================================= */

struct audio_in_impl {
    /* 配置 */
    audio_in_config_t        cfg;
    int                      frame_samples;  /* 每帧 sample 数 */

    /* AFE ring buffer（由 wake_word.c 提供） */
    RingbufHandle_t          ringbuf;

    /* VAD 状态机 */
    vad_state_t              vad_state;
    int16_t                 *vad_buf;       /* 累积当前句子的 PCM */
    size_t                   vad_buf_cap;
    size_t                   vad_buf_len;
    int64_t                  speech_start_ms;
    int64_t                  last_speech_ms;

    /* 能量阈值滑动窗口 */
    int32_t                  energy_sum;
    int32_t                  energy_count;
    int32_t                  energy_avg;

    /* Opus 编码器 */
    void                    *opus_enc;
    int16_t                  opus_pcm[320];
    size_t                   opus_samples;

    /* 打断补传只统计成功发给 WebSocket 的 Opus 包，不把“读出缓存”当作上传成功。 */
    wake_word_preroll_info_t  preroll;
    size_t                   preroll_packets;

    /* 任务 */
    TaskHandle_t             task_h;
    SemaphoreHandle_t        done_sem;
    atomic_bool              running;
    uint32_t                 connection_generation;

    /* 回调 */
    audio_in_pcm_cb_t        pcm_cb;
    void                    *pcm_cb_arg;
    audio_in_utterance_cb_t  utt_cb;
    void                    *utt_cb_arg;

    /* 互斥 */
    SemaphoreHandle_t        lock;
};

/* ===========================================================================
 * 工具函数
 * ========================================================================= */

static int32_t calc_frame_energy(const int16_t *pcm, int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; i++) {
        int32_t s = pcm[i];
        sum += (int64_t)s * s;
    }
    return (int32_t)(sum / samples);  /* 均方值 */
}

static void vad_reset(audio_in_handle_t h)
{
    h->vad_buf_len     = 0;
    h->vad_state       = VAD_STATE_IDLE;
    h->speech_start_ms = 0;
    h->last_speech_ms  = 0;
    h->energy_sum      = 0;
    h->energy_count    = 0;
    h->energy_avg      = 0;
}

/* ===========================================================================
 * VAD 状态机
 * ========================================================================= */

static void process_vad_frame(audio_in_handle_t h, const int16_t *pcm,
                              size_t pcm_samples, uint32_t now_ms)
{
    int32_t energy = calc_frame_energy(pcm, pcm_samples);
    h->energy_sum += energy;
    h->energy_count++;

    /* 滑动平均 */
    if (h->energy_count >= 8) {
        h->energy_avg = h->energy_sum / h->energy_count;
        h->energy_sum  = 0;
        h->energy_count = 0;
    }

    bool is_speech = (h->energy_avg > h->cfg.vad_threshold);

    switch (h->vad_state) {
        case VAD_STATE_IDLE:
            if (is_speech) {
                h->vad_state       = VAD_STATE_SPEECH;
                h->speech_start_ms = now_ms;
                h->last_speech_ms  = now_ms;
                h->vad_buf_len     = 0;
                ESP_LOGD(TAG, "VAD: SPEECH start (energy=%ld)", (long)energy);

                /* 事件 */
                event_bus_publish(EV_AUDIO_SPEECH_START, NULL, 0);
            }
            break;

        case VAD_STATE_SPEECH:
            if (is_speech) {
                h->last_speech_ms = now_ms;
            }

            /* 累积 PCM */
            if (h->vad_buf != NULL && h->vad_buf_len + pcm_samples * 2 <= h->vad_buf_cap) {
                memcpy(h->vad_buf + h->vad_buf_len / 2,
                       pcm, pcm_samples * 2);
                h->vad_buf_len += pcm_samples * 2;
            }

            /* 静默超时 */
            if (!is_speech && (now_ms - h->last_speech_ms) >= h->cfg.silence_timeout_ms) {
                h->vad_state = VAD_STATE_SILENCE;
                ESP_LOGD(TAG, "VAD: silence timeout, len=%zu", h->vad_buf_len);
            }
            break;

        case VAD_STATE_SILENCE:
            /* 累积尾部 PCM */
            if (h->vad_buf != NULL && h->vad_buf_len + pcm_samples * 2 <= h->vad_buf_cap) {
                memcpy(h->vad_buf + h->vad_buf_len / 2, pcm, pcm_samples * 2);
                h->vad_buf_len += pcm_samples * 2;
            }
            break;
    }

    /* SPEECH/SILENCE → IDLE（发送句子） */
    if (h->vad_state == VAD_STATE_SILENCE) {
        int64_t speech_dur_ms = h->last_speech_ms - h->speech_start_ms;

        if (speech_dur_ms >= h->cfg.min_utterance_ms && h->vad_buf_len > 0) {
            ESP_LOGI(TAG, "VAD: utterance done, len=%zu bytes, dur=%lld ms",
                     h->vad_buf_len, speech_dur_ms);

            if (h->utt_cb) {
                int16_t *copy = malloc(h->vad_buf_len);
                if (copy) {
                    memcpy(copy, h->vad_buf, h->vad_buf_len);
                    h->utt_cb(h->utt_cb_arg, copy, h->vad_buf_len);
                    free(copy);
                }
            }

            event_bus_publish(EV_AUDIO_SPEECH_END, NULL, 0);
        }

        vad_reset(h);
    }
}

/* ===========================================================================
 * Opus 编码（每帧实时编码 + WS 发送）
 * ========================================================================= */

static void fail_audio_upload(audio_in_handle_t h, esp_err_t err)
{
    if (!h->running) return;
    ESP_LOGW(TAG, "Audio upload failed: %s; requesting recovery", esp_err_to_name(err));
    h->running = false;
    event_bus_publish(EV_AUDIO_UPLOAD_FAILED, (void *)(uintptr_t)h->connection_generation, 0);
}

static void account_preroll_packet(audio_in_handle_t h)
{
    if (!h->preroll.id) return;
    ++h->preroll_packets;
    /* PCM 帧为 32 ms、Opus 包为 20 ms，最后一包可能同时包含缓存尾部和实时音频。
     * 以成功发送的采样数跨过快照边界为准，不要求两种帧大小整除。
     */
    if (h->preroll_packets * 320u < h->preroll.samples) return;
    size_t dropped = wake_word_finish_preroll(h->preroll.id);
    ESP_LOGI(TAG, "Barge-in replay sent: id=%lu audio_ms=%u opus_frames=%u dropped_ms=%u",
             (unsigned long)h->preroll.id,
             (unsigned)audio_preroll_samples_to_ms(h->preroll.samples),
             (unsigned)h->preroll_packets, (unsigned)audio_preroll_samples_to_ms(dropped));
    h->preroll.id = 0;
}

static void encode_and_send(audio_in_handle_t h, const int16_t *pcm, size_t pcm_samples)
{
    if (h->opus_enc == NULL) {
        opus_enc_config_t cfg = {
            .sample_rate = OPUS_SAMPLE_RATE,
            .channels    = OPUS_CHANNELS,
            .complexity  = OPUS_COMPLEXITY,
            .bitrate     = ACOS_OPUS_BITRATE,
            .frame_ms    = 20,
        };
        h->opus_enc = opus_enc_create(&cfg);
        if (h->opus_enc == NULL) {
            ESP_LOGW(TAG, "opus encoder create failed");
            fail_audio_upload(h, ESP_ERR_NO_MEM);
            return;
        }
    }

    /* Opus 编码 */
    uint8_t opus_out[256];
    for (size_t i = 0; i < pcm_samples && h->running; ++i) {
        h->opus_pcm[h->opus_samples++] = pcm[i];
        if (h->opus_samples == 320) {
            int opus_len = opus_enc_encode(h->opus_enc, h->opus_pcm, sizeof(h->opus_pcm), opus_out, sizeof(opus_out));
            if (opus_len > 0) {
                esp_err_t err = ws_service_send_opus_frame(opus_out, opus_len);
                if (err == ESP_OK) account_preroll_packet(h);
                else fail_audio_upload(h, err);
            } else {
                /* 编码失败不能跳过一句话中的采样后继续报告“完整补传”。 */
                ESP_LOGW(TAG, "Opus encode failed: %d", opus_len);
                fail_audio_upload(h, ESP_FAIL);
            }
            h->opus_samples = 0;
        }
    }
}

/* ===========================================================================
 * ring buffer 读取任务
 * ========================================================================= */

static void audio_in_task(void *param)
{
    audio_in_handle_t h = (audio_in_handle_t)param;
    const size_t item_size = wake_word_get_chunksize() * sizeof(int16_t);
    int16_t *frame_buf = malloc(item_size);

    if (frame_buf == NULL) {
        ESP_LOGE(TAG, "frame_buf malloc failed");
        fail_audio_upload(h, ESP_ERR_NO_MEM);
        xSemaphoreGive(h->done_sem);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "audio_in task started (ringbuf, chunksize=%d, core %d)",
             wake_word_get_chunksize(), xPortGetCoreID());

    h->preroll_packets = 0;
    if (wake_word_get_preroll_info(&h->preroll)) {
        ESP_LOGI(TAG, "Barge-in replay start: id=%lu pcm_frames=%u audio_ms=%u",
                 (unsigned long)h->preroll.id, (unsigned)h->preroll.frames,
                 (unsigned)audio_preroll_samples_to_ms(h->preroll.samples));
    }

    while (h->running) {
        /* 从 ring buffer 读取 PCM（阻塞，最长 100ms） */
        size_t item_size_out = 0;
        int16_t *buf = (int16_t *)xRingbufferReceive(h->ringbuf, &item_size_out,
                                                      pdMS_TO_TICKS(100));
        if (buf == NULL) {
            /* 超时：ring buffer 空，继续等待 */
            continue;
        }

        size_t samples_read = item_size_out / sizeof(int16_t);
        uint32_t ts = (uint32_t)(esp_log_timestamp());

        /* 先复制并归还 ring buffer 项，再编码/发送；网络阻塞时不占着环形缓存的归还位置。 */
        if (item_size_out > item_size) {
            vRingbufferReturnItem(h->ringbuf, buf);
            fail_audio_upload(h, ESP_ERR_INVALID_SIZE);
            break;
        }
        memcpy(frame_buf, buf, item_size_out);
        vRingbufferReturnItem(h->ringbuf, buf);

        /* PCM 回调 */
        if (h->pcm_cb) {
            h->pcm_cb(h->pcm_cb_arg, frame_buf, item_size_out, ts);
        }

        /* Opus 编码 + WS 发送（实时） */
        encode_and_send(h, frame_buf, samples_read);

        /* VAD */
        if (h->running) process_vad_frame(h, frame_buf, samples_read, ts);
    }

    if (h->preroll.id) {
        size_t dropped = wake_word_finish_preroll(h->preroll.id);
        ESP_LOGW(TAG, "Barge-in replay incomplete: id=%lu sent_opus_frames=%u target_ms=%u dropped_ms=%u",
                 (unsigned long)h->preroll.id, (unsigned)h->preroll_packets,
                 (unsigned)audio_preroll_samples_to_ms(h->preroll.samples),
                 (unsigned)audio_preroll_samples_to_ms(dropped));
        h->preroll.id = 0;
    }
    free(frame_buf);
    ESP_LOGI(TAG, "audio_in task exited");
    /* 通知 audio_in_stop() 任务已退出；不能使用 xTaskNotifyGive(NULL)，
     * 因为该 IDF 配置下 NULL 不是有效的通知目标。 */
    xSemaphoreGive(h->done_sem);
    vTaskDelete(NULL);
}

/* ===========================================================================
 * 公共 API
 * ========================================================================= */

audio_in_config_t audio_in_default_config(void)
{
    return (audio_in_config_t){
        .sample_rate         = BSP_I2S_SAMPLE_RATE,
        .frame_ms            = 32,             /* 与 AFE 帧对齐 */
        .mic_gain_db         = 30,
        .vad_threshold       = 500,
        .silence_timeout_ms  = VAD_SILENCE_TIMEOUT_MS,
        .min_utterance_ms    = VAD_MIN_UTTERANCE_MS,
        .bus                 = NULL,
    };
}

audio_in_handle_t audio_in_create(const audio_in_config_t *cfg)
{
    audio_in_handle_t h = calloc(1, sizeof(struct audio_in_impl));
    if (h == NULL) return NULL;

    h->cfg = cfg ? *cfg : audio_in_default_config();
    h->frame_samples = h->cfg.sample_rate * h->cfg.frame_ms / 1000;

    /* VAD 缓冲区: 最多 20 秒语音 */
    h->vad_buf_cap = h->cfg.sample_rate * 20 * 2;
    h->vad_buf = heap_caps_malloc(h->vad_buf_cap, MALLOC_CAP_8BIT);
    if (h->vad_buf == NULL) {
        ESP_LOGE(TAG, "vad_buf alloc failed");
        free(h);
        return NULL;
    }

    h->lock = xSemaphoreCreateMutex();
    if (h->lock == NULL) {
        free(h->vad_buf);
        free(h);
        return NULL;
    }

    h->done_sem = xSemaphoreCreateBinary();
    if (h->done_sem == NULL) {
        vSemaphoreDelete(h->lock);
        free(h->vad_buf);
        free(h);
        return NULL;
    }

    ESP_LOGI(TAG, "audio_in created (no I²S — uses AFE ring buffer), "
             "frame_samples=%d, vad_threshold=%d",
             h->frame_samples, h->cfg.vad_threshold);
    s_audio_handle = h;
    return h;
}

void audio_in_destroy(audio_in_handle_t h)
{
    if (h == NULL) return;
    if (audio_in_stop(h) != ESP_OK) return;

    if (h->opus_enc) {
        opus_enc_destroy(h->opus_enc);
        h->opus_enc = NULL;
    }

    free(h->vad_buf);
    vSemaphoreDelete(h->done_sem);
    vSemaphoreDelete(h->lock);
    if (s_audio_handle == h) s_audio_handle = NULL;
    free(h);
}

audio_in_handle_t audio_in_get_handle(void)
{
    return s_audio_handle;
}

esp_err_t audio_in_start_from_ringbuf(audio_in_handle_t h, RingbufHandle_t ringbuf)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    if (h->running) return ESP_OK;
    if (ringbuf == NULL) return ESP_ERR_INVALID_ARG;
    if (h->task_h) {
        esp_err_t err = audio_in_stop(h);
        if (err != ESP_OK) return err;
    }

    h->ringbuf = ringbuf;
    h->running = true;
    h->connection_generation = ws_service_generation();
    h->opus_samples = 0;
    /* 清除上一次任务的完成信号，再创建新任务。 */
    xSemaphoreTake(h->done_sem, 0);

    /* VAD 状态复位 */
    vad_reset(h);

    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_in_task, "audio_in",
        TASK_STACK_AUDIO_IN,
        h, 5, &h->task_h, 1);   /* core 1，与 detect_Task 同一核，共享 L2 */
    if (ret != pdPASS) {
        h->running = false;
        h->task_h = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t audio_in_stop(audio_in_handle_t h)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;

    h->running = false;
    if (h->task_h) {
        /* ring buffer 读取最多阻塞 100 ms，任务会自行退出并释放信号量。 */
        if (xSemaphoreTake(h->done_sem, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "audio_in task did not exit within timeout");
            /* The task still owns the encoder. Never free/reuse it early. */
            return ESP_ERR_TIMEOUT;
        }
        h->task_h = NULL;
    }

    /* 停止 Opus 编码器 */
    if (h->opus_enc) {
        opus_enc_destroy(h->opus_enc);
        h->opus_enc = NULL;
    }

    h->opus_samples = 0;

    ESP_LOGI(TAG, "audio_in stopped");
    return ESP_OK;
}

esp_err_t audio_in_register_pcm_cb(audio_in_handle_t h,
                                   audio_in_pcm_cb_t cb, void *user_data)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    h->pcm_cb     = cb;
    h->pcm_cb_arg = user_data;
    return ESP_OK;
}

esp_err_t audio_in_register_utterance_cb(audio_in_handle_t h,
                                         audio_in_utterance_cb_t cb,
                                         void *user_data)
{
    if (h == NULL) return ESP_ERR_INVALID_ARG;
    h->utt_cb     = cb;
    h->utt_cb_arg = user_data;
    return ESP_OK;
}

vad_state_t audio_in_get_vad_state(audio_in_handle_t h)
{
    if (h == NULL) return VAD_STATE_IDLE;
    return h->vad_state;
}

uint32_t audio_in_get_stack_hiwat(audio_in_handle_t h)
{
    if (h == NULL || h->task_h == NULL) return 0;
    return uxTaskGetStackHighWaterMark(h->task_h);
}
