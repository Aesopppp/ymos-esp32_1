# 智能语音辅助设备 — 技术设计文档（TDD）

> 项目代号：**KorvoAgent-P0/P1**
> 硬件平台：**ESP32-S3-Korvo-2 V3.1.2**（ESP32-S3-WROOM-1，8 MB Flash + 8 MB PSRAM）
> 软件基线：**ESP-IDF v5.4.4** + **esp_codec_dev v1.4.x**（IDF Component Manager） + **esp-adf-misc**（Opus） + **FreeRTOS**
> 文档版本：**v0.3（ESP-IDF v5.4.4 升级版，P0+P1 合并开发）**
> 编写日期：2026-09-17
> 关联文档：
> - PRD：`PRD-智能语音辅助设备.md`（v0.3）
> - 评审清单：`PRD-评审Checklist.md`（v0.2）
> - 协议基线：`ACOS API 对接V1.md`（v1.2，2026-06-27）
> - 工具链：`D:\work\ESP32IDF\ESP32IDF5_4\esp-idf\export.ps1`

---

## 0. 术语表

| 术语 | 含义 |
|------|------|
| PCM | Pulse Code Modulation，原始未压缩音频采样 |
| Opus | 开源有损音频编解码；ACOS V1 标准上行/下行编码 |
| Barge-In | 语音打断：用户说话时立即停止 AI 播放 |
| WW | Wake Word，唤醒词（默认"你好小易"）|
| VAD | Voice Activity Detection，语音活动检测 |
| AEC | Acoustic Echo Cancellation，回声消除 |
| jitter buffer | 抖动缓冲，平滑发包间隔 |
| NVS | Non-Volatile Storage，ESP32 的键值持久化分区 |
| wss | WebSocket over TLS |
| OI | Open Issue，PRD 中待确认项 |
| ASR | Automatic Speech Recognition，语音转写 |
| RAG | Retrieval-Augmented Generation，知识检索增强 |
| LLM | 大语言模型，生成推荐话术 |
| esp_codec_dev | ESP-IDF IDF Component Manager 音频编解码抽象组件（v1.4.x）|
| esp-adf-misc | ESP-ADF 工具库，含 esp_opus 编解码器 |

---

## 1. 概述

### 1.1 产品定位（与 PRD §1.2 对齐）

坐席侧的语音采集与播放终端，对接客服辅助平台 **ACOS Platform**：
- 客户端麦克风采集客户语音 → 上行至 ACOS → ACOS 转写并生成 AI 回复音频 → 下行回设备播放给坐席收听。

### 1.2 P0 / P1 分期范围

| 阶段 | 设备职责 | 关键能力 |
|------|---------|---------|
| **P0（本期）** | 纯收音终端（单向） | 唤醒 → MIC 采集 PCM → Opus 编码 → WebSocket JSON 上行；LED 指示；OTA；配置管理 |
| **P1（下期）** | 收音 + 播放（双向） | 接收 ACOS 下行 Opus 二进制帧 → ES8311 DAC + NS4150 播放；Barge-In 打断；音量控制；播放状态反馈 |

> 本 TDD v0.3 **P0 + P1 合并开发**，一次性实现完整链路。

### 1.3 软件基线变更（v0.2 → v0.3）

> ⚠️ **v0.3 核心变更：ESP-IDF v5.2.8 + ESP-ADF → ESP-IDF v5.4.4 + esp_codec_dev**

| 维度 | v0.2（v5.2.8 + ESP-ADF）| **v0.3（v5.4.4 + esp_codec_dev）** |
|------|--------------------------|--------------------------------------|
| IDF 版本 | v5.2.8 | **v5.4.4** |
| codec 组件 | ESP-ADF pipeline | **esp_codec_dev v1.4.x**（IDF Component Manager）|
| Opus 来源 | ESP-ADF esp_opus | **esp-adf-misc**（IDF Component Manager）|
| 驱动 API | esp_driver_i2s（legacy）| **esp_driver_i2s v5.4**（I2S std/tdm 分离）|
| I²C 驱动 | i2c_driver | **i2c_master**（v5.4 新 API）|
| websocket | espwebsocket | **esp_websocket_client**（ESP-IDF 内置）|
| esp_sr | esp_afe_sr_v1（ADF）| **esp_sr v4.x**（独立组件，需验证 v5.4.4 兼容性）|

### 1.4 协议基线（与 ACOS API V1 对齐）

| 维度 | 值 |
|------|---|
| 端点 | `wss://acos-platform.emicloud.com/acos-realtime`（可配置）|
| 鉴权 | 连接后首条 JSON `{type:"auth", token:"sk-..."}` |
| 上行编码 | **Opus** + base64（JSON 文本帧，`encoding:"opus"`）|
| 下行编码 | **Opus 二进制帧**（WebSocket opcode `0x2`，60ms/帧，24kbps）|
| 控制帧 | `__proxy_connected__ / speech_started / speech_stopped / response.audio.done / error` |
| Barge-In | `speech_started` → 立即停止播放 + 清空队列 |
| 心跳 | 隐含在 WS ping/pong + TCP keepalive |

### 1.5 设计原则

1. **静态优先**：所有长生命周期对象走静态分配或一次性预分配，避免运行期 malloc。
2. **流式不阻塞**：采集（I²S DMA）、编码（Opus）、传输（WS）三段独立任务，队列解耦。
3. **可降级**：单麦/双麦、wss/ws、Opus/PCM（透传）、扬声器/耳机，任一环节异常都有 fallback。
4. **可观测**：本地串口 + ACOS 端通过 `event_id` 关联日志。
5. **协议向前兼容**：JSON 帧新增字段忽略未知；帧类型未实现也保持解析器能识别。

---

## 2. 硬件平台与外设映射（V3.1.2）

### 2.1 主控资源

| 项 | 值 | 说明 |
|----|---|------|
| SoC | ESP32-S3-WROOM-1（N8R8）| Xtensa LX7 双核 240 MHz，8 MB Quad Flash，8 MB PSRAM（外挂）|
| 内置 SRAM | 512 KB | 实际可用约 400 KB |
| PSRAM | 8 MB | jitter buffer / 播放队列 / Opus 编解码缓冲 |
| Wi-Fi | 802.11 b/g/n | 仅 2.4 GHz |

### 2.2 V3.1.2 板载外设引脚映射（原理图确认）

| 外设 | 芯片 | 总线 | GPIO | 信号名（原理图）| P0 用途 | P1 扩展 |
|------|------|------|------|-----------------|---------|---------|
| I²C SDA | ES7210/ES8311 | I²C0 | **GPIO1** | ES_I2C_SDA | I²C 配置 | 同 |
| I²C SCL | ES7210/ES8311 | I²C0 | **GPIO2** | ES_I2C_CLK | I²C 配置 | 同 |
| I²S0 MCLK | — | — | **GPIO16** | Codec_I2S0_MCLK | 主时钟 | 同 |
| I²S0 BCLK | — | — | **GPIO9** | Codec_I2S0_SCLK | 位时钟 | 同 |
| I²S0 LRCK | — | — | **GPIO45** | Codec_I2S0_LRCK | 字时钟 | 同 |
| I²S0 DIN（ES7210）| ES7210 | I²S0 | **GPIO10** | Codec_I2S0_DSDIN | ADC 数据输入 | 同 |
| I²S0 DOUT（ES8311）| ES8311 | I²S0 | **GPIO8** | Codec_I2S0_DSDOUT | DAC 数据输出 | P1 DAC |
| PA 使能 | NS4150 | GPIO | **GPIO48** | PA_CTRL | — | P1 扬声器使能 |
| 按键阵列 | — | ADC | **GPIO5** | BT_ARRAY_ADC | REC/MUTE/SET 等 | 同 |
| 外设电源 | — | GPIO | **GPIO18** | PERI_PWR_ON | 电源控制 | 同 |
| 绿 LED | TCA9554 | I²C | **I²C 扩展** | GP_LED1 | Listening 指示 | + Playing 常亮 |
| 红 LED | TCA9554 | I²C | **I²C 扩展** | GP_LED2 | 异常指示 | 同 |
| SDMMC D0 | — | SDMMC | **GPIO34** | SD_DAT0 | 预留 | 预留 |
| SDMMC CLK | — | SDMMC | **GPIO36** | SD_CLK | 预留 | 预留 |
| SDMMC CMD | — | SDMMC | **GPIO35** | SD_CMD | 预留 | 预留 |
| SDMMC DET | — | SDMMC | **GPIO37** | SD_DET | 预留 | 预留 |
| UART TX | — | — | **GPIO43** | — | 日志/烧录 | 同 |
| UART RX | — | — | **GPIO44** | — | 日志/烧录 | 同 |

> ⚠️ **GPIO10/ES7210_SDOUT 与 SDMMC_DAT1 共用**：V3.1.2 原理图显示 GPIO10 同时是 SDMMC_D0。**本项目不使用 SDMMC**（SD 卡录音暂不实现），GPIO10 专用于 ES7210 I²S DIN，安全。

> ⚠️ **TCA9554 I²C 扩展**：绿/红 LED **非直连 GPIO**，通过 I²C 扩展芯片控制（I²C 地址待查原理图确认）。TDD v0.2 的 GPIO 直连 LED 设计废弃。

> ⚠️ **I²C 总线**：ES7210 + ES8311 + TCA9554 **共用同一组 I²C GPIO1/2**，通过不同设备地址区分。

### 2.3 资源预算（关键约束）

| 资源 | 预算 | 说明 |
|------|------|------|
| 固件总大小 | **< 4 MB** | app + bootloader + partitions + esp_sr 模型 + esp_opus |
| 内置 SRAM（业务）| < 200 KB | 音频 + Opus + 网络 + 任务栈 + 协议 |
| PSRAM | 充足 | jitter / 播放队列 / Opus 编解码缓冲 |
| CPU 占用（Listening+Playing）| < 70%（单核）| 双核分工：核0 网络/Opus，核1 音频 |

---

## 3. 软件架构

### 3.1 分层与模块

```
┌──────────────────────────────────────────────────────────────┐
│  Application Layer（应用层 / 状态机）                           │
│   - agent_state.c   设备状态机：Sleep/Listening/Playing/Config/OTA/Fault
│   - event_bus.c     事件分发                                   │
│   - barge_in.c      语音打断协调器（P1）                        │
├──────────────────────────────────────────────────────────────┤
│  Service Layer（服务层）                                       │
│   - audio_in.c        I²S0 + ES7210 采集（esp_codec_dev）      │
│   - audio_out.c【P1】 I²S0 DOUT + ES8311 + NS4150 播放        │
│   - opus_codec.c      Opus 编码器（P0上行）+ 解码器（P1下行）   │
│   - ww_service.c      唤醒词检测（esp_sr）                    │
│   - vad_service.c     VAD + 静音帧策略（esp_sr AFE）           │
│   - ws_service.c      WebSocket 客户端 + 重连（esp_websocket）│
│   - proto_service.c   ACOS 帧编解码 + 文本/二进制分发          │
│   - wifi_service.c    STA + NTP                               │
│   - cfg_service.c     NVS + JSON 配置                         │
│   - led_service.c     LED 状态机（TCA9554 I²C 扩展）          │
│   - ota_service.c     HTTP OTA                                │
│   - log_service.c     日志缓冲 + 串口                         │
│   - watchdog.c        TWDT + Panic Handler                   │
├──────────────────────────────────────────────────────────────┤
│  Driver / HAL（驱动 / 硬件抽象）                               │
│   - esp_driver_i2s      ESP-IDF v5.4 I²S std 模式            │
│   - esp_driver_i2c       I²C master 总线                      │
│   - es7210_driver       通过 esp_codec_dev 抽象               │
│   - es8311_driver【P1】 通过 esp_codec_dev 抽象               │
│   - ns4150_driver【P1】 GPIO EN + 软件静音                    │
│   - tca9554_driver      I²C IO 扩展驱动（LED）                │
│   - nvs_driver          ESP-IDF NVS                          │
│   - wifi_driver         ESP-IDF esp_wifi                     │
├──────────────────────────────────────────────────────────────┤
│  ESP-IDF v5.4.4 / FreeRTOS / esp_codec_dev v1.4.x / esp-adf-misc │
└──────────────────────────────────────────────────────────────┘
```

### 3.2 FreeRTOS 任务划分

| 任务名 | 优先级 | 核心 | 栈（KB）| 职责 |
|--------|--------|------|--------|------|
| `task_audio_in` | 24 | 1 | 8 | I²S0 DMA 回调 → rb_pcm_raw |
| `task_ww` | 20 | 1 | 12 | esp_sr 唤醒词检测 |
| `task_vad_opus` | 18 | 1 | 16 | VAD + jitter + Opus 编码 → q_ws_out |
| `task_audio_out`【P1】 | 22 | 1 | 12 | I²S0 DOUT DMA 播放 + 消费 playback_q |
| `task_opus_dec`【P1】 | 19 | 1 | 16 | 二进制帧 → Opus 解码 → playback_q |
| `task_ws` | 16 | 0 | 12 | WebSocket 收发、重连、心跳 |
| `task_proto_rx` | 15 | 0 | 6 | 解析下行 JSON 帧 |
| `task_barge_in`【P1】 | 21 | 0 | 4 | 监听 speech_started → 协调 audio_out + playback_q |
| `task_wifi_ntp` | 10 | 0 | 6 | Wi-Fi 事件 + NTP 同步 |
| `task_led` | 5 | 0 | 2 | LED 状态机（TCA9554）|
| `task_log_flush` | 5 | 0 | 4 | 日志刷串口 |
| `task_ota` | 5 | 0 | 8 | OTA 升级（窗口控制）|
| `task_monitor` | 3 | 0 | 4 | 看门狗喂狗 + 内存巡检 |

队列与缓冲：

| 队列/缓冲 | 大小 | 位置 | 说明 |
|-----------|------|------|------|
| `rb_pcm_raw` | 512 × 4 帧 | PSRAM | I²S0 → ww/vad |
| `rb_pcm_play`【P1】 | 960 × 8 帧（~480ms）| PSRAM | 解码后 PCM → I²S0 DOUT |
| `q_ws_out` | 32 帧 | 内置 | JSON 上行帧 |
| `q_ws_in_text` | 16 帧 | 内置 | 文本下行 |
| `q_ws_in_bin`【P1】 | 64 包 | PSRAM | 二进制下行 |
| `q_event` | 32 事件 | 内置 | 内部事件总线 |

### 3.3 关键数据流

**P0（上行单向）**：

```
MIC(ES7210) → I²S0 DMA → rb_pcm_raw → ww_service
                                    │
                                    └─(WW 命中)→ event: WW_HIT → app_state: Sleep→Listening

rb_pcm_raw → vad → jitter → opus_encode → JSON encode → q_ws_out → ws_service ──► wss ACOS
```

**P1（双向，引入下行播放与打断）**：

```
                                                                                   ┌────► audio_out（ES8311 + NS4150）
                                                                                   │        │
                                                                                   │        ▼
上行链路（P0 同）                                                                 │   扬声器/耳机
                                                                                   │
ws_service ──(binary)──► q_ws_in_bin ──► opus_decode ──► playback_q ───────────────┘
                       (text)        proto_service
                            │
                            ├── __proxy_connected__ → app_state: WS_CONN
                            ├── speech_started      → barge_in.task ──► stop audio_out + clear playback_q
                            ├── speech_stopped       → app_state: 记录 utterance 结束
                            ├── response.audio.done  → app_state: 标记本轮 TTS 完成
                            └── error                → app_state: 错误处理
```

---

## 4. 关键模块设计

### 4.1 音频采集通路（ES7210 + I²S0 + DMA）— P0

**目标**：16 kHz / 16 bit / 单声道 / 单左麦；CPU 占用 < 25%。

**组件依赖**：
```yaml
# main/idf_component.yml
dependencies:
  espressif/esp_codec_dev: ">=1.4.0,<1.5.0"
```

**初始化序列**：

1. `i2c_master_new_bus()`（GPIO1/2）：I²C 总线初始化
2. `i2s_new_channel(I2S_MODE_RX)`：创建 I²S0 接收通道（std philips 模式，非 TDM）
3. `i2s_channel_config()`：采样率 **16000**，位深 16，slot `I2S_SLOT_MONO`
4. `es7210_codec_new()` via `esp_codec_dev`：
   - 麦克风选择：`ES7210_SEL_MIC1`（单左麦）
   - 增益：可配置，默认 30 dB（`esp_codec_dev_set_in_gain`）
   - 主时钟：ESP32-S3 MCLK = 16000 × 256 = **4.096 MHz**
5. `esp_codec_dev_open()`：配置 ES7210 到活动状态
6. `i2s_channel_enable()`：启动 DMA 接收
7. `task_audio_in` 通过轮询或 DMA 回调读取 PCM：`esp_codec_dev_read(codec, buf, len)`

**V3.1.2 引脚配置**：

| 参数 | 值 |
|------|---|
| I²C 总线 | I2C_NUM_0 |
| SDA | GPIO1 |
| SCL | GPIO2 |
| I²S 通道 | I2S_NUM_0 |
| MCLK | GPIO16 |
| BCLK | GPIO9 |
| WS（LRCK）| GPIO45 |
| DIN（ES7210→ESP32）| GPIO10 |

**降级路径**：

| 异常 | 错误码 | 处理 |
|------|--------|------|
| ES7210 I²C 无应答 | `E_ADC_002` | 重试 3 次；连续失败 3 次则上报 Fault |
| DMA buffer 溢出 | `E_ADC_003` | 丢弃 1 帧 + 计数；连续 ≥ 10 次降采样到 8 kHz |

### 4.2 唤醒词引擎（esp_sr）— P0

**选型**：`esp_sr` 组件（需验证 v5.4.4 兼容性），唤醒词模型"你好小易"。

> ⚠️ **待验证**：v5.4.4 中 `esp_afe_sr_v1.create()` 等 API 可能在 esp_sr v4.x 中有变更。
> 备选方案：如 esp_sr v4.x API 不兼容，降级为纯 VAD 检测（`esp_sr_vad`），唤醒词改为 APP 端触发。

- `task_ww` 周期：每 30 ms 从 `rb_pcm_raw` 取一帧送入 AFE
- AFE 输出 (raw, raw_channel, vad, wake) → `ww_service` 仅消费 `wake` 标志
- 命中后：
  1. 发 `event(WW_HIT)`
  2. 保留 500 ms 前导缓冲
  3. `app_state` 切换 `Sleep → Listening`
  4. 绿色 LED 快闪 3 次（TCA9554）

### 4.3 VAD + 静音帧策略 — P0

**VAD 实现**：复用 esp_sr AFE 的 VAD 输出，参数：

| 参数 | 默认值 | 可配置（NVS）|
|------|--------|------------|
| 静音超时（ms）| 800 | 400~3000 |
| 前导缓冲（ms）| 500 | 固定 |

**静音期间发包策略**：持续发送低速率 Opus 静音帧（约 10 bytes），保证服务端 VAD 与 session 状态连续。

### 4.4 Jitter Buffer + Opus 编码 — P0

**目标**：把 VAD 输出平滑为固定 20 ms Opus 帧节拍。

**实现**：
- 深度 3 帧（约 60 ms），可配置 NVS `jitter_depth_frames`
- 节拍来源：`esp_timer` 周期回调（每 20 ms）
- 编码：`opus_codec.encode(pcm320, &out, &out_len)` → base64 → 拼接 JSON
- 超过深度的新帧：丢弃 + 计数
- 低于水位：发送 Opus 静音帧

### 4.5 Opus 编解码器（esp_adf_misc）— P0/P1

**组件**：
```yaml
# main/idf_component.yml
dependencies:
  espressif/esp_adf_misc: "~1.0"
```

**编码参数**（P0 上行）：

| 项 | 值 | 说明 |
|----|---|------|
| 应用类型 | VOIP | 低延迟优化 |
| 码率 | 24 kbps | ACOS V1 推荐 |
| 复杂度 | 5 | 平衡 CPU / 质量 |
| 帧长 | 20 ms / 320 样本 | ACOS 推荐 |
| 采样率 | 16000 Hz | 固定 |
| 声道 | 1 | 固定 |

**解码参数**（P1 下行）：

| 项 | 值 | 说明 |
|----|---|------|
| 码率 | 24 kbps | ACOS V1 固定 |
| 帧长 | 60 ms / 960 样本 | ACOS V1 固定 |
| FEC | 启用 | 抗丢包 |
| PLC | 启用 | 丢包隐藏 |
| 输出采样率 | 16000 Hz | 与 I²S 一致 |

### 4.6 WebSocket 客户端 — P0

**选型**：ESP-IDF 内置 `esp_websocket_client`（基于 mbedTLS）。

**连接参数**：

| 项 | 值 | 说明 |
|----|---|------|
| URL | `wss://acos-platform.emicloud.com/acos-realtime` | 从 NVS 读取 |
| 鉴权 | 首条 JSON：`{type:"auth", token:"sk-..."}` | 不走 HTTP Header |
| TLS | 强制 wss | CA 证书内置 |
| 超时 | 连接 10 s / 无数据空闲 90 s | |
| 缓冲 | 发送 4 KB / 接收 8 KB | |

**重连策略**：

```
idle → connecting → connected
                  ↓ 失败
              retry_wait(t = {3,5,10,10,10...}s)
                  ↓ 10 次后
              fault_state（LED 错误指示）
```

### 4.7 协议层（proto_service，对齐 ACOS V1）— P0/P1

**帧类型覆盖**：

| 方向 | 帧 | 类型 | 状态 |
|------|-----|------|------|
| C→S | 鉴权 | `auth` | P0 |
| C→S | 上行音频 | `input_audio_buffer.append` | P0 |
| S→C | 就绪 | `__proxy_connected__` | P0 |
| S→C | 检测到说话 | `speech_started` | P1（Barge-In）|
| S→C | 检测到静音 | `speech_stopped` | P1 |
| S→C | AI 回复结束 | `response.audio.done` | P1 |
| S→C | AI 回复音频 | **二进制帧**（opcode `0x2`）| P1 |
| S→C | 错误 | `error` | P0 |

**上行音频帧**：
```json
{
  "event_id": "event_{timestamp}_{random}",
  "type": "input_audio_buffer.append",
  "audio": "//uQxAAAAAANIAAAAAE...",
  "encoding": "opus"
}
```

### 4.8 音频播放通路（ES8311 + NS4150）— P1

**组件**：ES8311 通过 `esp_codec_dev` 抽象，NS4150 通过 GPIO48 控制使能。

**V3.1.2 硬件连接**：
- ES8311 DAC 输出 → NS4150 功放输入 → 扬声器
- GPIO48 控制 NS4150 使能（高电平使能）
- ES8311 和 ES7210 **共用同一 I²S0 总线**（DIN=DATA_IN, DOUT=DATA_OUT）

**播放队列（Barge-In 支持）**：
- `playback_q`（FreeRTOS Queue，64 帧，PSRAM）
- `barge_in` 协调器：收到 `speech_started` 时：
  1. `i2s_channel_stop()` 立即停止 DMA
  2. `xQueueReset(playback_q)` 清空队列
  3. GPIO48 → 低（关闭 PA）
  4. 等待 `speech_stopped` 后恢复

### 4.9 LED 服务（TCA9554）— P0

> ⚠️ V3.1.2 原理图显示绿/红 LED 通过 TCA9554 I²C 扩展芯片控制，**非直连 GPIO**。

**TCA9554 I²C 地址**：待从原理图确认（通常 0x3E 或 0x3F）。

**LED 状态机**：

| 状态 | 绿 LED | 红 LED |
|------|--------|--------|
| 启动中 | 快闪（200ms 周期）| 灭 |
| Sleep | 慢闪（2s 周期）| 灭 |
| Listening | 常亮 | 灭 |
| Playing（P1）| 常亮 | 灭 |
| Wi-Fi 连接中 | 灭 | 快闪 |
| 错误/Fault | 灭 | 快闪（500ms 周期）|

---

## 5. sdkconfig 关键配置

### 5.1 目标与分区

```
CONFIG_IDF_TARGET=esp32s3
CONFIG_IDF_TARGET_ESP32S3=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_ESPTOOLPY_FLASH_SIZE_8MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
```

### 5.2 PSRAM

```
CONFIG_ESPTOOLPY_APP_SEPARATE=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SIZE=8388608
CONFIG_SPIRAM=y
CONFIG_SPIRAM_BOOTLOADER_LOAD_IN_RAM=y
CONFIG_SPIRAM_APP_MEM_ON=y
CONFIG_SPIRAM_ALLOW_BSS_EXTERNAL_MEMORY=y
```

### 5.3 FreeRTOS

```
CONFIG_FREERTOS_SMP=0
CONFIG_FREERTOS_UNICORE=n
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_IDLE_TASK_STACKSIZE=2048
```

### 5.4 组件版本（IDF Component Manager）

```
# main/idf_component.yml
dependencies:
  idf:
    version: ">=5.4.0"
  espressif/esp_codec_dev: ">=1.4.0,<1.5.0"
  espressif/esp_adf_misc: "~1.0"
```

---

## 6. 错误码体系

| 错误码 | 名称 | 级别 | 说明 |
|--------|------|------|------|
| `E_ADC_002` | ES7210 无应答 | ERROR | I²C 通信失败 |
| `E_ADC_003` | DMA 溢出 | WARN | 帧丢弃，可能降级 |
| `E_WS_001` | WS 连接失败 | ERROR | 网络或平台问题 |
| `E_WS_002` | WS 断开 | WARN | 自动重连 |
| `E_WS_003` | ACOS 鉴权失败 | ERROR | token 无效 |
| `E_OPUS_001` | Opus 编码失败 | ERROR | 固件 BUG |
| `E_OPUS_002` | Opus 解码失败 | WARN | 帧损坏，降噪 |
| `E_LED_001` | TCA9554 无应答 | WARN | LED 功能降级 |
| `E_DAC_001` | ES8311 无应答 | ERROR | P1 播放不可用 |

---

## 7. Open Issues（OI）

| OI | 描述 | 状态 | 负责人 |
|----|------|------|--------|
| OI-1 | TCA9554 I²C 地址待从 V3.1.2 原理图确认 | OPEN | 待查 |
| OI-2 | esp_sr v4.x API 与 v5.4.4 兼容性待验证 | OPEN | 开发期验证 |
| OI-3 | GPIO10 SDMMC 共用是否影响 ES7210 I²S0 DIN | CLOSED | V3.1.2 不启用 SDMMC，安全 |
| OI-4 | ES8311 I²S DOUT 与 ES7210 DIN 共用 I²S0 总线时序 | CLOSED | 分时复用，demo 验证可行 |
| OI-5 | 唤醒词模型"你好小易"是否包含在 esp_sr 组件中 | OPEN | 需下载模型文件 |

---

*本文档由 MiniMax Code（Mavis）基于 ESP-IDF v5.4.4 + V3.1.2 原理图生成*
