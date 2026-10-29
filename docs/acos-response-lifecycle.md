# ACOS 工具调用与多段回复的本地兼容处理

适用工程：`D:\ESP32-ymos-1`；ESP32-S3-Korvo-2 V3.1；ESP-IDF 6.1。

## 对接依据与问题证据

本次结合 `docs/ACOS API 对接V1.md` 与此前提供的 `ACOS MQTT对接.md`，修复设置、查询闹钟后出现的回复中断与后续对话异常。两条链路需要分别理解：

- 普通对话中“设置闹钟”“查询闹钟数量”属于平台模型/工具调用，仍走原 WebSocket 语音链路，不应被本地识别为提醒播放。
- 到点播报走 MQTT 提醒队列，设备向已认证 WebSocket 原样转发消息，等待平台返回语音。规则调度、工具执行和语音生成由平台负责。

已提供的异常日志中，出现过 `response.done` 比首个音频包先到，以及同一回合第一段结束后继续收到第二段音频的顺序。旧实现把普通 `response.done completed` 当作音频结束，可能在尚未收到音频时就恢复监听；也会在首个 `audio.done` 后拒收后续段。一次记录为 `decoded=76 discarded=100`，说明问题不能仅归因于网络或缓存容量。

### 2026-10-28 闹钟问答复测

本次复测日志的 ELF SHA256 前缀为 `24a5bf087`，与上轮交付固件匹配。日志中的 `LISTENING -> PLAYING` 也代表等待平台回复，不能据此认定已收到声音。

- 约 71.5 秒用户语音结束；72～114 秒连续收到不同 ID 哈希的工具阶段结束事件，策略一直为 `phase=1 tool_pending=1`（等待回复），116.6 秒按原 45 秒上限恢复会话。这段没有成功接受音频的状态证据。需要平台核查工具调用是否重复、执行结果是否回填、是否启动最终语音生成；日志不足以确认闹钟是否已创建。
- 约 174.9 秒再次进入工具等待；180.7 秒已有 `Playback stats`，说明设备确实解码并向播放链路提交了音频，不能把这一轮归为平台完全无音频。182.4 秒收到 `response.audio.done`，但仍为 `phase=2 tool_pending=1`。原策略只靠可选 created/message 清除工具等待，漏掉 V1 原始 Opus 直接续流，导致排空后仍无法恢复监听。

本轮修复：**只有当前有效回合成功通过音频接收门时，才解除工具等待，并等待新的音频 EOF 与排空**。之后若再次出现工具阶段，仍重新等待。无音频的工具结束、旧回合、已取消/丢弃或已完成回合都不能借此完成或重新开流。保留原超时，不自动重发可能已生效的闹钟创建请求，也不在本地伪造“设置成功”的回复。

## 文档约定与可选扩展

ACOS V1 明确约定：上行是 Base64 Opus JSON，16 kHz、单声道、推荐 20 ms；下行是无标识头的原始 Opus 二进制，16 kHz、单声道、60 ms。**本轮正常音频结束由 `response.audio.done` 表示**。

V1 文档未定义 `response.done`、工具调用生命周期或显式取消确认。本地兼容下表扩展，不能据此认为平台一定发送这些事件：

| 事件 | 本地解释 |
| --- | --- |
| `response.audio.done` | 当前音频段 EOF；等待队列及扬声器排空，不立即恢复监听 |
| `response.done`，`status=completed` | 模型/工具阶段结束；普通播放不据此设置音频 EOF |
| `response.done`，输出含 `function_call` / `tool_call` | 仍等待工具后续，不能把工具完成当作最终语音完成 |
| `response.created`，带 `response.id` 或 `response_id` | 可识别的新回复阶段；绑定 ID，清除上一段 EOF/工具等待 |
| `response.output_item.added/done`，条目为工具 | 标记工具等待 |
| `response.function_call_arguments.done` | 标记工具等待 |
| `response.output_item.added`，条目为 `message` | 进入语音消息阶段，解除工具等待并清除旧 EOF |
| `response.output_item.done`，条目为 `message` | 解除工具等待，保留此前音频 EOF，避免尾部事件让设备一直等待 |
| 当前有效回合的原始 Opus 二进制 | 开始/继续语音，解除此前工具等待并清除旧 EOF；不依赖可选 created/message |
| `response.done`，`failed/incomplete/cancelled` | 在普通回复中报告失败，由主任务恢复会话 |
| 未知但格式有效的事件 | 忽略，不把扩展事件当作连接故障 |

只有取消等待流程，才兼容 `response.audio.done`、明确 cancelled/canceled 或非工具的 `response.done completed` 作为结束边界；既有“无活动回复可取消”错误也沿用原处理。**`speech_started`、`speech_stopped` 都不是取消确认**。平台若不提供可信边界，仍按原超时策略受控重建连接。

## 本地职责和并发约束

保留原 C 服务模块、事件总线、主任务状态机和音频工作任务，不新增业务线程或扩大音频缓冲。

| 文件 | 责任 |
| --- | --- |
| `main/proto_service.c/.h` | 使用工程已依赖的 cJSON，按 JSON 对象层级读取事件、response 和 error；结果持有有界副本 |
| `main/response_stream.c/.h` | 小型回复流策略，管理回合、阶段 ID、工具等待、音频 EOF、排空和取消 |
| `main/ws_service.c` | 网络接收、FIFO 入队、工作任务解码与事件发布；在播放互斥锁下调用策略 |
| `main/audio_out.c` | 同一回复后续段撤销 EOF 时保留未播 PCM/DMA 尾音 |
| `main/app_main.c` | 明确后续阶段到达时暂停麦克风上传，复用原等待/播放流程；继续处理恢复与超时 |
| `tests/response_stream_checks.c` | 直接调用生产解析器与策略，重放故障事件顺序 |

关键约束：

1. `turn` 隔离用户回合；有明确 `response.created` 后，控制事件中的响应 ID 必须匹配当前阶段。无 ID 的旧版 V1 消息仍兼容，不凭空给原始二进制添加身份。
2. 收到音频 EOF 后，只要回复还在 WAITING/PLAYING，可接收同一回合的后续二进制段，解除工具等待、撤销 EOF 并继续解码；不以工具阶段的 completed 代替真实音频到达。
3. 完成条件同时要求：音频 EOF、无工具等待、接收 FIFO 空、网络端仍处于音频结束边界、没有完整帧正在分发、扬声器排空。完成事件只发布一次。
4. 阶段控制的 ID 校验在工作任务执行，网络回调只观察音频进度，不能让一个被忽略的迟到控制事件撤销当前音频边界。
5. 已排空回合只接受带不同 ID 的明确 `response.created` 续播；重复 created、上一阶段 ID、用户已开始下一次发言后的迟到 created 均不重开。处于取消或丢弃状态时拒绝音频。
6. 等待取消确认期间，迟到 `speech_stopped` 不推进网络回合号，避免随后真正取消边界被误判为旧回合。
7. 网络发送不持播放锁；解码仍在接收工作任务。WebSocket 任务栈显式设置为 6144 字节，容纳有界字段和结构化解析；没有改动全局 cJSON 分配钩子。

JSON 解析只按顶层 `type`、`response.status/id/output`、`error.code/message` 读取，不再全文搜索同名键；工具参数中的 `type/status` 不参与协议判断。拒绝重复键、超过 12 层嵌套、解码 NUL、尾随垃圾、冲突或超长 ID；响应 ID 最多 127 字节。解析树在每次解析结束前释放，字段副本不借用其地址。任务栈增加约 2 KiB，cJSON 节点仍有瞬时堆开销，需在实板检查工具大消息时的最低堆余量。

阶段日志只包含事件名、回合号、ID 哈希、固定白名单状态类别、工具标记、音频 EOF、成功解码/丢弃计数及队列长度，常规忽略事件最多每 5 秒记录一次。不输出工具参数、原始 MQTT 消息或凭据。

新增诊断：`Reply audio accepted` 仅在回合首帧或工具后的首帧成功解码时记录，`after_tool=1` 表示已从工具等待续入音频；`Reply pending` 在普通回复超时恢复前记录一次一致快照。若超时摘要为 `decoded=0 tool_pending=1`，结合阶段日志交平台排查；若已有解码帧，则结合 `eof/drained/queue/wire_done` 进一步判断结束边界与排空条件，不能只看 PLAYING。

## 主机回归与构建

在工程根目录使用 C11 主机编译器执行。`work/` 为已忽略的本地输出目录，`tests/host` 只为未联网的帧构造函数提供时间/随机接口声明：

```powershell
tcc -std=c11 -Wall -Werror -Itests/host -Imanaged_components/espressif__cjson/cJSON -o work/response-stream-checks.exe tests/response_stream_checks.c main/proto_service.c main/response_stream.c managed_components/espressif__cjson/cJSON/cJSON.c
& .\work\response-stream-checks.exe

tcc -std=c11 -Wall -Werror -Imanaged_components/espressif__cjson/cJSON -o work/reminder-message-checks.exe tests/reminder_message_checks.c main/reminder_message.c managed_components/espressif__cjson/cJSON/cJSON.c
& .\work\reminder-message-checks.exe
```

当前结果：369 项回复流检查、513 项提醒检查通过。回复流回归包括：

- completed 先于首包音频；嵌套工具字段不会被误认为顶层事件。
- 76 帧音频、audio.done、completed、100 帧音频、audio.done，全部接收 176 帧。
- 工具等待、新响应阶段、旧 ID 结束迟到、重复 created、消息项在音频之后结束。
- 工具结束后不发 created/message、直接返回 Opus 与 audio.done；以及只有工具 created、没有后续 reply created 的兼容链路。先确认旧实现失败，再验证修复后可排空完成。
- 工具阶段无音频不得完成；音频后再次调用工具仍等待；续流仍需新的 EOF；旧回合/取消/丢弃音频不能解除等待或重开。
- 取消后音频隔离、旧回合丢弃、取消期间不推进回合号、非取消状态误调用结束接口无副作用。
- failed/incomplete 恢复，以及非法 JSON、字段所有权和边界校验。

在本工程 ESP-IDF 6.1 环境执行原有编译期断言和完整固件构建：

```powershell
xtensa-esp32s3-elf-g++ -std=c++17 -Wall -Wextra -Werror -fsyntax-only tests/audio_preroll_checks.cc tests/barge_in_guard_checks.cc tests/button_policy_checks.cc
idf.py build
```

编译期断言只验证预录音、打断保护和按键纯逻辑；主机回归不模拟 FreeRTOS 调度、DMA、声学条件或 ACOS 服务。

上轮 ESP-IDF 6.1 构建产物为 2,059,824 字节，ELF SHA256 前缀 `24a5bf087`；验证记录保留在 `work/response-lifecycle-verification.json`，用于对应用户本次日志。

2026-10-28 本轮工具后原始音频续流修复构建通过，产物为 `build/korvo_agent.bin`，大小 2,060,880 字节，3 MiB 应用分区剩余 34%。ELF SHA256 为 `1acf3fb7afc194bc45d9fe797c240efdf1f2e18e3df89448c6082e6f23eb9fdf`；BIN SHA256 为 `86d8732a140806544303cd3b00e24093ece1f6567eae24e040d8100cc612c939`。增量构建可能保留 app_desc 的旧编译时间，核对固件时应使用 ELF 哈希。受影响源文件的修改前副本、失败/通过回归、构建和验证记录均在 `work/tool-audio-eof-20261028/`。`work/` 不提交到版本库。

## 实板验收与平台边界

本次未烧录、未进行 ACOS 端到端联调。将成功构建的新固件烧录到目标板后，建议按原故障顺序检查：

1. 唤醒后普通聊天，确认一问一答、回复完整，完成后恢复监听。
2. “帮我设置一个闹钟”→“查询闹钟数量”→普通问题，确认每步能继续，长答复不截尾，空闲 60 秒回等待唤醒。
3. 查询闹钟的回复播放中语音打断，确认取消边界、句首补传和新问题回答；再测 REC 手动打断。
4. 测试无音频、平台工具失败、断网、迟到控制事件，确认超时恢复后仍能再次对话。
5. 平台完成提醒音频后，独立测试 MQTT → 原样 WS 转发 → 直接播放 → 排空回待机；不要把模型工具回复与到点提醒混为一个状态。
6. 观察 `Response stage`、`Playback drained`、`Cancel pending`，结合 AFE 统计和内部 RAM/PSRAM 最低余量判断。不要仅凭灯快闪认定网络断开，它也可能表示正在监听/等待回复。

平台仍需保证二进制音频和最终 `response.audio.done` 的顺序。如果一段已完全排空，平台隔很久才发无身份的后续二进制，本地不会凭它自动续播，否则会重新播放打断后的旧音频；需要平台提供明确的后续阶段或可靠的最终音频边界。若平台只返回工具结果而不下发音频、音频结束或取消确认，本地恢复不能替代平台修复。

本次保持本工程连接策略：待机关闭 WebSocket，保持 Wi-Fi/MQTT；对话与提醒按需连接。唤醒词模型、AEC/VAD、引脚和打断阈值未在本轮调整。
