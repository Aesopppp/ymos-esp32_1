# ymos-esp32

基于 ESP-IDF 的 ESP32-S3-Korvo-2 V3.1 语音终端固件，面向 ACOS 实时语音平台。项目参考小智 ESP32 工程的模块化设计，重点支持低延迟语音采集、唤醒、播放和 MQTT 控制面。

## 主要功能

- **语音唤醒**：使用 ESP-SR WakeNet，默认模型为 `wn9s_nihaoxiaozhi`。
- **语音采集**：ES7210 双麦克风、16 kHz 单声道 PCM，支持 AFE VAD 和回声处理。
- **语音播放**：ES8311 + NS4150，接收 ACOS 下行 Opus 并通过 I2S 播放。
- **实时通信**：通过 WSS 连接 ACOS，上传 Base64 Opus JSON 音频帧，接收二进制 Opus 音频帧。
- **连续对话**：唤醒后支持连续交互，默认静默超时 60 秒。
- **Barge-in 打断**：播放过程中检测用户说话，可取消当前回复并恢复收音。
- **NVS 配置**：保存 Wi-Fi、ACOS、MQTT、设备身份和运行参数。
- **MQTT 控制面**：Wi-Fi 获取 IP 后启动 MQTT，支持 LWT、在线状态、能力上报、命令和配置主题订阅。
- **设备身份**：首次启动读取 ESP32 MAC，生成并持久化 `chip_id`、`device_id` 和默认 `client_id`。

当前 MQTT 主要用于状态和控制，实时音频仍通过 ACOS WebSocket 传输。SoftAP/HTTP 用户配网、MQTT 下行播报命令和定时提醒仍在开发中。

## 硬件与软件要求

### 硬件

- ESP32-S3-Korvo-2 V3.1
- ES7210 音频 ADC / 麦克风
- ES8311 音频 DAC
- NS4150 功放和扬声器
- 16 MB Flash、8 MB Octal PSRAM 目标配置

### 软件

- ESP-IDF 6.1.x
- Xtensa ESP32-S3 GCC 工具链
- Git for Windows（需要 `patch.exe`）
- ESP-IDF 提供的 Python 环境

## 安装与构建

### 获取代码

```powershell
git clone https://github.com/zhangff-2021/ymos-esp32.git
cd ymos-esp32
```

### 设置 ESP-IDF

打开 ESP-IDF 6.1 PowerShell，或执行对应版本的 `export.ps1`：

```powershell
idf.py --version
```

### 配置凭据

编辑 [main/board_config.h](main/board_config.h)，填写开发阶段默认值：

```c
#define DEFAULT_WIFI_SSID     "你的WiFi名称"
#define DEFAULT_WIFI_PASSWORD "你的WiFi密码"
#define DEFAULT_ACOS_TOKEN    "你的ACOS_TOKEN"
```

不要提交真实密码、Token、MQTT 密码或证书。设备已经保存过 NVS 配置时，NVS 优先级高于编译期默认值。

本机测试若需要替换设备已经保存的 Wi-Fi，可创建 `main/wifi_local_config.h`
（已加入 Git 忽略规则），填写：

```c
#pragma once
#define LOCAL_WIFI_PRESET_ID 1U
#define LOCAL_WIFI_SSID "你的热点名称"
#define LOCAL_WIFI_PASSWORD "你的热点密码"
```

重新编译并烧录后，首次启动会将该预设写入 NVS，替换旧的 Wi-Fi 凭据；
不会清除 ACOS、MQTT 或设备身份。相同预设编号只执行一次，之后网页配网保存的新网络仍生效。
下次需要用固件切换网络时，修改凭据并更换为未使用过的预设编号。
未提供本机配置文件时保持原有行为；移除文件不会删除已经写入设备的 NVS 配置。

MQTT 配置字段包括 `mqtt_enabled`、`mqtt_host`、`mqtt_port`、`mqtt_client_id`、`mqtt_username`、`mqtt_password` 和 `mqtt_base_topic`。

### 编译

```powershell
idf.py build
```

首次编译会下载 managed components，需要网络连接。项目使用自定义分区表 [partitions.csv](partitions.csv)，请保留 `sdkconfig`、`sdkconfig.defaults` 和分区表。

## 烧录

将开发板连接电脑，确认串口号，例如 `COM3`：

```powershell
idf.py -p COM3 flash monitor
```

退出监视器使用 `Ctrl+]`。首次安装建议完整烧录 bootloader、分区表、应用和模型相关分区，不能只烧录单独的应用 bin 文件。

## 使用方法

### 首次启动

1. 初始化 NVS、音频编解码器、WakeNet 和 Wi-Fi。
2. 读取 NVS 中的 Wi-Fi 凭据；没有保存值时使用 `board_config.h` 默认值。
3. Wi-Fi 获得 IP 后启动 MQTT，发布在线状态和 `hello` 能力消息。
4. 设备保持等待唤醒状态，不会自动建立 ACOS WebSocket。

### 语音对话

1. 对设备说唤醒词。
2. 设备建立 ACOS WebSocket 并发送鉴权消息。
3. 麦克风数据经过 AFE/VAD 和 Opus 编码后上传 ACOS。
4. ACOS 返回 Opus 音频，设备解码并播放。
5. 播放完成后恢复收音；静默超时后关闭会话并等待下一次唤醒。

播放期间持续采集并保留约 1.5 秒预录音，确认打断后补传句首及取消等待期间的音频。
打断判定允许最多 96 ms 的短暂弱音或停顿，要求 640 ms 内累计 480 ms 达标人声；
同时保留播放起始 1200 ms 保护和能量门限。
模块职责、诊断日志和测试方法见 [打断预录音与补传](docs/打断预录音与补传.md)。

### MQTT 状态和主题

默认主题前缀为 `ymos/v1`：

```text
ymos/v1/device/{device_id}/status
ymos/v1/device/{device_id}/event
ymos/v1/device/{device_id}/command
ymos/v1/device/{device_id}/config/set
ymos/v1/device/{device_id}/config/ack
```

MQTT 连接成功后会发布 retained 的 `online` 状态、注册 `offline` 遗嘱消息、发布设备能力并订阅命令和配置主题。当前已完成基础连接和命令事件投递，`wake_and_speak`、配置 ACK、消息去重和定时提醒仍需继续接入主状态机。

### 查看日志

```powershell
idf.py -p COM3 monitor
```

重点关注 `app_main`、`cfg_service`、`wifi_service`、`mqtt_service`、`ws_service` 和 `audio_out` 标签。

## 项目结构

```text
main/
  app_main.c        主任务、状态机和事件分发
  cfg_service.*     NVS 配置和设备身份
  wifi_service.*    Wi-Fi STA 连接与状态事件
  mqtt_service.*    MQTT 连接、LWT、订阅和状态上报
  mqtt_topics.*     MQTT 主题生成
  ws_service.*      ACOS WebSocket 客户端
  proto_service.*   ACOS JSON 协议解析与构造
  audio_in.c        麦克风、AFE、VAD 和 Opus 上行
  audio_out.c       Opus 解码、PCM 缓冲和扬声器播放
  wake_word.*       WakeNet 唤醒词
components/         本地组件声明
docs/               协议、设计和功能计划
partitions.csv      Flash 分区表
sdkconfig*          ESP-IDF 构建配置
```

## 注意事项

- 当前工程固定面向 ESP32-S3-Korvo-2 V3.1，不保证其他开发板可直接使用。
- ACOS Token、Wi-Fi 密码和 MQTT 凭据属于敏感信息，不要提交到公共仓库。
- MQTT 默认配置是开发占位值；生产环境应使用 TLS、证书校验和安全凭据存储。
- SoftAP/HTTP 配网尚未完成，修改 Wi-Fi 时可使用编译期默认值或现有 NVS 配置。
- 播放打断、AEC 和不同网络环境下的延迟仍需在真实 Korvo-2 硬件上验证。

## 相关文档

- [NVS、Wi-Fi 配网和 MQTT 功能完善计划](docs/功能完善计划-NVS-WiFi配网-MQTT.md)
- [ACOS API 对接文档](docs/ACOS API 对接V1.md)
- [技术设计文档](docs/技术设计文档-TDD.md)
