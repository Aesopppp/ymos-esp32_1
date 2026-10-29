# ACOS 远程定时语音提醒

## 功能与协议边界

适用工程：`ESP32-ymos-1`，目标板 ESP32-S3-Korvo-2 V3.1，ESP-IDF 6.1。

定时规则、药品名称和提醒内容由 ACOS 平台管理。设备在线接收 MQTT 提醒，把消息原始 JSON 字节转发到 ACOS WebSocket，接收平台返回的二进制 Opus，复用现有解码、PCM 缓冲和扬声器播放链路。

```text
ACOS 平台定时触发
    → MQTT：acos/<企业 API Key>/remind
    → 设备校验、去重、排队
    → 待机时建立 ACOS WebSocket 并认证
    → 原样发送 MQTT 消息正文
    → 二进制 Opus + response.audio.done
    → 扬声器排空后结束提醒，关闭 WebSocket，回到等待唤醒
```

本版不增加本地时钟调度、HTTP 音频下载或本地文字转语音。MQTT 中的 `data.text` 是平台协议字段；只收到文字、没有平台音频下行时，设备不会自行合成语音。

与参考工程的功能要求一致，但沿用本工程的连接策略：**待机保持 Wi-Fi 和 MQTT，WebSocket 只在对话或提醒需要时连接**。提醒跳过 LISTENING，不启动麦克风上传；AFE 仍用于既有的播放打断判定。

## 两个 MQTT 通道

| 通道 | 用途 | 配置与生命周期 |
| --- | --- | --- |
| `mqtt_service` | 原 YMOS 在线状态、LWT、能力和控制主题 | 保留原 `mqtt_*` 配置与行为 |
| `acos_reminder_service` | 订阅 ACOS 提醒 | 独立 `rm_*` 配置；不依赖原 `mqtt_enabled` 开关 |

两个客户端使用不同 Client ID。提醒客户端 ID 为 `ymos-reminder-<STA MAC>`，MQTT 3.1.1、QoS 1、keepalive 60 秒、自动重连间隔 5 秒。Wi-Fi 获得 IP 后启动；进入配网或 Wi-Fi 断开时停止并销毁网络客户端，保留 RAM 中尚未过期的排队和去重记录。

提醒使用 clean session，平台应发送 **retain=false** 的消息。设备拒绝 retained 消息，避免重新订阅时重复播放旧提醒。离线期间没有持久订阅补发保证；需要补发时由平台按有效期重新投递。

## 配置方法

### 编译期配置

复制 `main/acos_reminder_local_config.h.example` 为同目录的 `acos_reminder_local_config.h`，按平台实际参数修改。后者已加入 Git 忽略规则：

```c
#define ACOS_REMINDER_ENABLED true
#define ACOS_REMINDER_URI "mqtt://47.94.139.172:11116"
#define ACOS_REMINDER_API_KEY ""
#define ACOS_REMINDER_USE_WS_TOKEN true
#define ACOS_REMINDER_USERNAME ""
#define ACOS_REMINDER_PASSWORD ""
```

API Key 为空且 `USE_WS_TOKEN=true` 时，读取现有 NVS `acos_token` 作为企业 Key 测试。配置独立 `API_KEY` 后优先使用独立 Key；它只用于 MQTT Topic 和消息收件人，不改变 WebSocket 鉴权 Token。企业 Key 是否与语音 Token 相同，仍需平台确认。

默认 Broker 地址来自现有 ACOS MQTT 对接文档。平台启用 TLS 后可改为 `mqtts://主机:端口`，校验使用 ESP-IDF 证书包。用户名、密码通过独立字段配置；不要把凭据嵌进 URI。

### NVS 配置与优先级

延续现有 `cfg_service`：**NVS 中已有值优先，键不存在时才使用编译期默认值**。不擦除旧配置，不修改已有键序号。独立 API Key 为空时，再按 `rm_token` 决定是否回退语音 Token。

NVS namespace 为 `korvo_cfg`：

| 配置枚举 | NVS 键 | 类型 | 默认值 |
| --- | --- | --- | --- |
| `CFG_KEY_REMINDER_ENABLED` | `rm_enabled` | bool | true |
| `CFG_KEY_REMINDER_URI` | `rm_uri` | string | `mqtt://47.94.139.172:11116` |
| `CFG_KEY_REMINDER_API_KEY` | `rm_api_key` | string | 空 |
| `CFG_KEY_REMINDER_USE_TOKEN` | `rm_token` | bool | true |
| `CFG_KEY_REMINDER_USERNAME` | `rm_username` | string | 空 |
| `CFG_KEY_REMINDER_PASSWORD` | `rm_password` | string | 空 |

需要后续管理页面时，可调用 `cfg_service_set_str/set_bool` 写入上述键；本次未增加设置页面或远程改密接口。更改配置后重新启动提醒服务或设备，使新连接使用新值。修改编译默认值不会覆盖已有 NVS 值。

Key 长度为 1～256 字节，只接受英文字母、数字、`-_.`，不允许 MQTT 通配符或路径分隔符。Broker URI、用户名、密码最多各 256 字节。日志不输出 Key、Topic、完整消息正文和密码。

## 下发消息

Topic 必须精确为 `acos/<企业 API Key>/remind`，收件人必须精确为 `device:<同一企业 API Key>`。两种消息使用相同 Topic：

| 字段 | 校验要求 |
| --- | --- |
| `v` | 整数 1 |
| `id` | 1～96 字节字符串；每次独立提醒使用唯一 ID |
| `ts` | Unix 秒级时间戳，正整数，最大 4102444800 |
| `from` | 1～128 字节字符串 |
| `to` | 精确匹配设备订阅 Key 对应的 `device:<Key>` |
| `type` | `remind` 或 `scheduled_reminder` |
| `data.text` | 1～1024 字节字符串 |
| `data.reminder_id` | `scheduled_reminder` 必填，正整数，最大 2^53−1 |
| `data.scheduled_for` | `scheduled_reminder` 必填，1～64 字节字符串；时间调度由平台负责 |

拒绝重复 JSON 键、嵌入 NUL、超过 8 层嵌套、尾部非空白内容或超过 8192 字节的消息。MQTT 分片必须连续、Topic 正确；完整校验后才入队。校验不重新序列化正文，转发保留原空白、字段顺序和转义。

下面用 PowerShell 生成测试正文，时间戳随运行更新。`test-key` 仅用于隔离测试，正式联调时须与设备实际配置一致：

```powershell
$reminderTs = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$reminderId = [Guid]::NewGuid().ToString()
$reminderJson = @"
{"v":1,"id":"$reminderId","ts":$reminderTs,"from":"reminder:3","to":"device:test-key","type":"scheduled_reminder","data":{"reminder_id":3,"text":"该吃午饭了","scheduled_for":"$([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))"}}
"@
```

通过平台或 MQTT 测试客户端把这段 UTF-8 正文发布到 `acos/test-key/remind`，QoS 1、retain=false。若手工复用旧示例，请更新 `ts` 和 `id`，避免过期或去重造成预期内的拒收。

平台收到设备转发的 JSON 后，需要在**同一已认证 WebSocket** 下发既有二进制 Opus 帧，并发送 `response.audio.done` 表示音频流结束。`response.done completed` 只表示模型/工具阶段结束，不能代替音频结束，否则提前到达的工具结果会截断后续语音。提醒路径不依赖先收到 `input_audio_buffer.speech_stopped`；多段音频与可选工具阶段兼容规则见 [回复生命周期说明](acos-response-lifecycle.md)。

## 排队、播放与恢复

| 场景 | 行为 |
| --- | --- |
| 待机且联网 | 取出最早的有效提醒，连接/认证 WebSocket，原样请求音频，跳过监听 |
| 正在对话、取消回复或恢复连接 | 先排队，等回到 WAIT_WAKE 且扬声器排空后播放 |
| 收到结束事件但 PCM 尚未播完 | 等扬声器实际排空，再记完成；不截断尾音 |
| 正常播放结束 | 记完成，关闭 WebSocket，回到等待唤醒；再检查下一条提醒 |
| 播放中语音打断或短按 REC | 复用原取消边界和预录音补传；本条记取消，转为普通对话 |
| 提醒连接/等待音频时短按 REC | 取消提醒，按原流程建立普通对话 |
| 长按 REC 结束、进入配网 | 取消当前提醒并释放资源；尚未取出的消息保留至过期 |
| 断网、服务端错误、空音频结束或超时 | 本条记失败，回到等待唤醒；不自动重播半段音频 |

提醒正常播放期间不启动 `audio_in`，WebSocket 上行接口也拒绝麦克风帧。只有用户打断并完成取消、转入普通对话后，才恢复麦克风上传。未改动原 AEC/VAD 参数、音频引脚和打断阈值。

资源与时间限制：

- 最多 8 条，包含正在处理的一条；满队列拒收新消息。
- 排队有效期 10 分钟。设备时间已校准到 2020 年以后时，额外拒绝 `ts` 超过过去 600 秒或未来 300 秒的消息；未校时仍按单调时钟限制排队时间。
- 完成/取消记录最多 128 个 ID，保留 1 小时；记录满时覆盖最旧项。排队中和正在播放的 ID 同样去重。
- 失败不加入完成去重表，平台可在有效期内用同一 ID 重新投递。设备不自行重试不确定是否已发送成功的请求。
- 连接与鉴权准备最多 20 秒；请求发出后连续 30 秒无音频/结束边界则失败。音频未结束时总计最多 5 分钟；收到结束边界后允许尾音排空，总时长上限 6 分钟。
- 队列/去重表约 13 KB、消息正文最多约 64 KB，放 PSRAM；MQTT 输入分片缓冲额外最多约 8 KB，解析节点与网络任务还有独立开销。
- 队列与去重仅在 RAM 中，重启丢失。本版没有新增播放结果 ACK 协议；MQTT QoS 1 不等于播放成功，也不提供跨重启的恰好一次播报保证。

## 模块职责与维护

| 文件 | 职责 |
| --- | --- |
| `main/acos_reminder_config.h` | 编译默认值与本机覆盖入口 |
| `main/reminder_message.c/.h` | 不依赖 RTOS 的协议校验、分片、有界队列、去重和过期 |
| `main/acos_reminder_service.c/.h` | MQTT 生命周期，回调校验入队，互斥保护队列 |
| `main/ws_service.c/.h` | 提醒接收回合、原样 JSON 转发、麦克风上传门控、音频接收/排空 |
| `main/reminder_playback_policy.h` | 可独立验证的提醒流超时规则 |
| `main/app_main.c` | 主任务串行协调提醒、对话、打断、配网和异常恢复 |
| `main/app_state.c/.h` | 追加 `APP_STATE_REMINDER_WAITING`，接到播放开始后复用 PLAYING |
| `main/cfg_service.c/.h` | 追加 NVS 配置键，保留原配置 |
| `tests/reminder_message_checks.c` | 直接调用生产解析、分片、队列和策略的主机回归 |

服务接口 `init/start/stop/take/finish` 只由主任务调用。MQTT 回调不调用 WebSocket、音频或状态切换接口。`take` 返回借用的 active 消息，`finish` 后指针失效；回调不能释放或改写 active。网络发送不持有队列锁或播放锁，沿用原有 WebSocket 连接代次过滤。

## 验证与联调

本次已通过 ESP-IDF 6.1 固件构建、513 项主机检查及原预录音/打断/按键编译期断言。**未烧录、未进行实板或 ACOS 音频下行联调**；编译和纯逻辑测试不证明平台已经返回音频。

主机测试需要 C11 编译器，例如 TinyCC；在工程根目录执行，`work` 为已忽略的本地输出目录：

```powershell
tcc -std=c11 -Wall -Werror -Imanaged_components/espressif__cjson/cJSON -o work/reminder-message-checks.exe tests/reminder_message_checks.c main/reminder_message.c managed_components/espressif__cjson/cJSON/cJSON.c
& .\work\reminder-message-checks.exe
```

输出应为 `PASS: 513 reminder checks (production parser/fragments/queue/playback policy)`。原有编译期检查与构建：

```powershell
xtensa-esp32s3-elf-g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/audio_preroll_checks.cc tests/barge_in_guard_checks.cc tests/button_policy_checks.cc
idf.py build
```

烧录新固件后，在目标 Korvo-2 上依次验收：

1. 联网后看到 `Connected; subscribing to reminder topic (QoS 1)`，随后 `Reminder subscription acknowledged`。待机时确认没有麦克风上传和自动打开的 WebSocket。
2. 发布一条有效提醒，依次观察 `Reminder queued`、`Reminder requested while idle`、`Reminder MQTT payload forwarded unchanged`、`Reminder playback started`、`Playback drained`、`Reminder completed`；确认实际声音完整，未先进入监听。
3. 使用同一 ID 重投，确认 `Reminder duplicate` 且不重播；用多个新 ID 检查顺序、容量与过期。
4. 对话期间发布提醒，确认不抢占对话；结束对话/60 秒静默待机后再播。
5. 在提醒播放中语音打断或按 REC，确认旧音频停止、取消完成后句首补传、能正常问答；长按 REC 或 SET 应按原功能结束/配网。
6. 让平台不返回音频，确认约 30 秒后 `reminder connection/audio stream timed out`、`Reminder failed` 并回待机；另测错误/空音频结束、断网重连，不应假记完成或卡死后续对话。
7. 回归原唤醒、连续对话、打断、音量记忆和配网；检查内部 RAM 与 PSRAM 余量。

如果只看到转发成功，随后约 30 秒无音频超时，应先检查平台是否实现提醒 JSON 到 Opus 下行的处理，以及消息收件人/鉴权是否一致。转发成功日志只代表本次 WebSocket send 返回成功，不代表平台已完成语音合成或设备已播出。
