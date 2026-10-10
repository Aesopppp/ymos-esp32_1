# 智能语音辅助设备 — 技术设计文档（TDD v0.3 — P1 强化设计）

> 本文档基于 v0.2 升级，新增 P1 详细化设计，引用 v0.2 对应章节。
> 文档版本：**v0.3**
> 升级日期：2026-09-17
> 升级人：（待填写）
> 关联文档：
> - 主 TDD：技术设计文档-TDD.md（v0.2，1028 行）
> - PRD：PRD-智能语音辅助设备.md（v0.3）
> - 评审清单：PRD-评审Checklist.md（v0.2）
> - 协议基线：ACOS API 对接V1.md（v1.2，2026-06-27）
> - 实时控制台抓取：https://realtimechat.emicloud.com/（ACOS Web 调试控制台，2026-09-17）

---

## v0.2 → v0.3 变更摘要

| # | 章节 | 变更类型 | 变更内容 | 触发原因 |
|---|------|---------|---------|---------|
| 1 | §A | 新增 | Barge-In 端到端时序图 + TWDT 监控 + mock 注入测试方法 | v0.2 §5.4 仅给  ≤ 50ms 目标，缺测量与降级方案 |
| 2 | §B | 新增 | 平台 VAD/静音参数下发通道（session.update 扩展）| Web 控制台观察 VAD 阈值/静音可调；v0.2 §4.3 仅 NVS |
| 3 | §C | 改写 | 播放状态反馈从 v1 暂不实现 → P1 必做（	ts_feedback 帧）| v0.2 §5.6 与 PRD §1.4.3 矛盾 |
| 4 | §D | 新增 | 协议扩展点（session.update / 	ts_feedback / device.metric）+ 电话通话二进制帧区分 | v0.2 §6 仅覆盖 ACOS V1 当前帧 |
| 5 | §E | 完善 | 状态机 P1：耳机插拔事件 + ConfigAP 入口 + 状态转移表 | v0.2 §7 状态机 P1 部分简化 |
| 6 | §F | 新增 | OI-21~26 + 里程碑 P1-4（v0.3 协议联调）| v0.2 OI 仅到 OI-20，里程碑 P1 共 4 周 |
| 7 | §G | 新增 | v0.3 风险表 R-13~17 | 协议扩展点风险 |
| 8 | §H | 更新 | 待补充内容清单（v0.3 增项） | 配套 v0.2 §15 |

---

## §A Barge-In 时序深化（增强 v0.2 §5.4）

> 引用：v0.2 §5.4 给出 Barge-In ≤ 50ms 目标；本节补充端到端时序、TWDT 监控、实测方法、失败兜底。

### A.1 端到端时序（毫秒级）

`
时间(ms)   事件源                  事件                                  累计延迟
─────────────────────────────────────────────────────────────────────────────
T+0        ACOS 服务端 VAD 触发    服务端发出 input_audio_buffer.speech_started
T+5~15     TCP/IP + TLS            网络传输                              +15
T+15~25    ESP-IDF WS 客户端        触发 WS_DATA_RX 回调                +10
T+25~27    proto_service           解析 JSON + opcode=0x1 分发         +2
T+27~28    xQueueSend              投递 BARGE_IN 事件到 task_barge_in   +1
T+28~29    task_barge_in           事件接收 + 状态判定                  +1
T+29~32    task_audio_out          暂停 I²S1 DMA + 清空 tx buffer      +3
T+32~33    task_opus_dec           停止解码 + 重置 decoder state       +1
T+33~34    playback_q.flush()      环形缓冲清零                         +1
─────────────────────────────────────────────────────────────────────────────
T+34       audio_out 实际静音点    (DAC 输出 0)                         总计 ≤ 50ms
`

### A.2 TWDT 监控方案

`c
// 注册 task_barge_in 到 TWDT，超时阈值 100ms（远小于 50ms 目标）
void task_barge_in(void *arg) {
    esp_task_wdt_add(NULL);
    while (1) {
        event_t ev;
        if (xQueueReceive(q_barge_in, &ev, portMAX_DELAY) == pdTRUE) {
            esp_task_wdt_reset();
            int64_t t_start = esp_timer_get_time();
            
            audio_out.pause();         // 必须 ≤ 10ms
            playback_q.flush();       // 必须 ≤ 5ms
            opus_dec.reset();         // 必须 ≤ 5ms
            
            int64_t elapsed_us = esp_timer_get_time() - t_start;
            if (elapsed_us > 30000) {
                LOG_E(barge_in, exceeded 30ms: %lld us, elapsed_us);
                metric.barge_in_overrun++;
            }
            
            event_post(EVENT_BARGE_IN_DONE);
            app_state_set(STATE_LISTENING);
        }
        esp_task_wdt_reset();
    }
}
`

### A.3 实测方法（mock server 注入）

`python
# mock_acos_barge_test.py — 部署在开发机，监听 device WS probe 端口
import asyncio, websockets, json, time

async def test_barge_in_latency():
    async with websockets.connect(ws://device-ip:8080/probe) as ws:
        # 1. 触发播放：发送 N 个下行二进制 Opus 帧
        for i in range(20):
            await ws.send(binary_opus_frame_60ms)
            await asyncio.sleep(0.06)
        
        # 2. 注入 speech_started，测量设备停止播放的延迟
        t0 = time.monotonic()
        await ws.send(json.dumps({
            type: input_audio_buffer.speech_started,
            event_id: test_barge_001
        }))
        
        # 3. 设备回传 PROBE_PLAYBACK_STOPPED（通过另一个端口或日志）
        # 接收并计算 Δt
        # 目标：P50 ≤ 50ms，P95 ≤ 80ms
`

**实测目标**：
- P50 ≤ 50ms
- P95 ≤ 80ms
- 失败率 < 0.1%

### A.4 失败兜底

- **audio_out.pause() 超时**：强制 i2s_driver_uninstall + einstall（已知耗时 ~80ms，可接受）
- **playback_q.flush() 死锁**：跳过 + 计数 E_BARGE_002，下次 BARGE_IN_DONE 强制清零
- **TWDT 触发**：写入 coredump + 上报 metric，下次 OTA 自检修复

---

## §B 平台 VAD/静音参数下发（增强 v0.2 §4.3 + §5.5）

> 引用：v0.2 §4.3 VAD 参数仅 NVS 读取；§5.5 音量仅本地按键。本节扩展平台下发通道。

### B.1 Web 控制台观察（2026-09-17 抓取 https://realtimechat.emicloud.com/）

| 参数 | 控制台范围 | 控制台默认 | 设备 NVS 默认 | 设备端映射 |
|------|----------|----------|--------------|----------|
| VAD 阈值 | -1.0 ~ 1.0 | 0.5 | NVS ad_level 1~5 | -1.0~1.0（线性映射）|
| 静音超时 | 200 ~ 6000 ms | 800 ms | NVS ad_silence_ms 400~3000 | 钳制到 [200, 6000] |
| 音量 | 0 ~ 100 | 70 | NVS olume 0~100 | 直接映射 |
| 唤醒词模型 | — | xiaoyi | NVS wake_model_id | 字符串比较 |

### B.2 协议扩展帧 session.update（v0.3 扩展点，ACOS V1 §6 未定义）

> **说明**：ACOS V1 §6 当前未定义此类帧。本扩展帧为 v0.3 设计，**服务端实现确认前设备发送后服务端可忽略**。设备需兼容服务端无响应场景。

`json
{
  type: session.update,
  session: {
    vad_threshold: 0.5,
    vad_silence_ms: 800,
    wake_model_id: xiaoyi_v1,
    volume: 70
  },
  event_id: event_session_update_001
}
`

| 字段 | 类型 | 必填 | 范围 | 说明 |
|------|------|------|------|------|
| 	ype | string | 是 | 固定 session.update | 帧类型 |
| session.vad_threshold | float | 否 | -1.0 ~ 1.0 | VAD 灵敏度（-1 不敏感，1 最敏感）|
| session.vad_silence_ms | int | 否 | 200 ~ 6000 | 静音超时 |
| session.wake_model_id | string | 否 | — | 唤醒词模型 ID（预留）|
| session.volume | int | 否 | 0~100 | 音量百分比 |
| event_id | string | 否 | — | 追踪 ID |

### B.3 设备处理逻辑

`c
void proto_on_session_update(cJSON *session) {
    // 1. 范围校验（越界静默忽略，不报错）
    cJSON *vt = cJSON_GetObjectItem(session, vad_threshold);
    if (vt && (vt->valuedouble < -1.0 || vt->valuedouble > 1.0)) {
        LOG_W(proto, vad_threshold out of range: %.2f, vt->valuedouble);
        return;
    }
    cJSON *silence = cJSON_GetObjectItem(session, vad_silence_ms);
    if (silence && (silence->valueint < 200 || silence->valueint > 6000)) {
        LOG_W(proto, vad_silence_ms out of range: %d, silence->valueint);
        return;
    }
    
    // 2. 应用到运行期服务
    if (vt) vad_service.set_threshold(vt->valuedouble);
    if (silence) vad_service.set_silence_ms(silence->valueint);
    
    // 3. 持久化到 NVS（覆盖设备默认值，下次启动沿用）
    if (vt) { nvs_set_float(cfg, vad_threshold, vt->valuedouble); }
    if (silence) { nvs_set_i32(cfg, vad_silence_ms, silence->valueint); }
    nvs_commit();
    
    // 4. 触发事件供其他模块订阅（如 LED service 同步提示）
    event_post(EVENT_SESSION_UPDATED);
    
    // 5. 回执（fire-and-forget，服务端可忽略）
    proto_send_session_ack(cJSON_GetObjectItem(session, wake_model_id));
}
`

### B.4 优先级：平台下发 > NVS > 出厂默认

`
优先级（高 → 低）：
1. 平台 session.update 下发（仅当会话期内有效，断开恢复 NVS 默认值）
2. NVS 配置（vad_threshold / vad_silence_ms / volume）
3. 出厂 JSON 默认
`

**v0.3 决策**：
- 会话期内：平台下发覆盖 NVS
- 断开连接：恢复 NVS 默认值（避免 粘性）
- 重连后：若服务端再次下发则重新覆盖；否则沿用 NVS

---

## §C 播放状态反馈必做（改写 v0.2 §5.6）

> 引用：v0.2 §5.6 标注 v1 暂不实现。**PRD §1.4.3 明确要求**：播放状态反馈（LED 变化、播放完成事件上报平台）。
> **v0.3 升级为 P1 必做**。

### C.1 LED 反馈（与 v0.2 §4.10 Playing 状态一致）

| 设备状态 | LED 表现 | 触发条件 |
|---------|---------|---------|
| Listening | 绿色常亮 | MIC 采集中 |
| **Playing** | 绿色常亮 + 慢闪 0.5Hz | AI 回复音频播放中 |
| Listening ⇄ Playing 切换 | LED 渐变 200ms | 切换瞬间避免闪烁 |

> 硬件 LED 资源有限，Listening 和 Playing 共用绿色 LED，靠频率区分。后续硬件若增加双色可分离。

### C.2 平台回执帧 	ts_feedback（v0.3 扩展，ACOS V1 未定义）

> 同 §B，本帧为扩展帧，**服务端未实现时可忽略**。

`json
// 1. 开始播放
{
  type: tts_feedback.playback_start,
  event_id: event_tts_fb_001,
  ts: 1694856000000
}

// 2. 播放完成
{
  type: tts_feedback.playback_done,
  event_id: event_tts_fb_003,
  ts: 1694856001500,
  reason: completed | barge_in | error
}
`

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| 	ype | string | 是 | 	ts_feedback.playback_start 或 	ts_feedback.playback_done |
| event_id | string | 否 | 追踪 ID |
| 	s | int | 是 | UTC ms（NTP 同步后）；未同步用 uptime_ms |
| eason | string | 仅 done | completed / arge_in / error |

> v0.3 不实现 	ts_feedback.playback_progress（P1 默认**不上报**，节省带宽）；P2 评估。

### C.3 上报触发时机

| 事件 | 帧 | 触发位置 |
|------|-----|---------|
| audio_out 开始 DMA 播放 | 	ts_feedback.playback_start | task_audio_out 启动时 |
| audio_out 自然结束 | 	ts_feedback.playback_done reason=completed | 队列空 + decoder 无新包 |
| Barge-In 触发 | 	ts_feedback.playback_done reason=barge_in | task_barge_in 完成时 |
| audio_out 欠喂/异常 | 	ts_feedback.playback_done reason=error | task_audio_out 异常回调 |

### C.4 服务端无响应兜底

- **fire-and-forget**：客户端发送后**不等响应**
- **连接已断开**：本帧进入 q_ws_out 队列但会被 ws_service 在重连时清空（避免堆积）
- **设备端不重试**：ACOS session 是短时性，无 ACK 重试语义

---

## §D 协议扩展点（增强 v0.2 §6）

> 引用：v0.2 §6 仅识别 ACOS V1 当前帧。本节扩展 P1 阶段可能新增的帧类型。

### D.1 ACOS V1 已有帧（设备已支持）

| 方向 | 帧 | 类型 | 状态 |
|------|-----|------|------|
| C→S | 鉴权 | uth | ✅ P0 |
| C→S | 上行音频 | input_audio_buffer.append | ✅ P0 |
| S→C | 就绪 | __proxy_connected__ | ✅ P0 |
| S→C | 检测到说话 | input_audio_buffer.speech_started | ✅ P0/P1 |
| S→C | 检测到静音 | input_audio_buffer.speech_stopped | ✅ P0/P1 |
| S→C | AI 回复结束 | esponse.audio.done | ✅ P1 |
| S→C | 错误 | error | ✅ P0 |
| S→C | AI 回复音频 | 二进制（opcode 0x2） | ✅ P1 |
| S→C | **电话通话音频** | 二进制（opcode 0x2） | ⚠️ v0.3 区分 |

### D.2 P1 扩展帧（v0.3 新增）

| 方向 | 帧 | 类型 | 优先级 | 说明 |
|------|-----|------|--------|------|
| S→C | 平台参数下发 | session.update | P1 中 | VAD / 静音 / 音量（详见 §B）|
| C→S | 播放开始回执 | 	ts_feedback.playback_start | P1 中 | 详见 §C |
| C→S | 播放完成回执 | 	ts_feedback.playback_done | P1 中 | 详见 §C |
| C→S | 设备指标上报 | device.metric | P1 高 | CPU/heap/RSSI（v0.2 §4.13 提到）|
| S→C | 设备指令（预留）| device.command | P2 | 远程操作设备（重启/日志抓取）|

### D.3 电话通话音频 vs AI 回复音频（v0.3 区分）

ACOS V1 §4 时序图标注：

`
│◀── 7. 二进制 Opus 音频 ─────│  ← 二进制帧 (AI 回复音频，多个)
│◀── 8. response.audio.done ───│  ← 文本帧 (JSON)
│◀── 9. 二进制 Opus 音频 ─────│  ← 二进制帧 (电话通话音频，可选)
`

两者**仅靠 esponse.audio.done 区分**：
- esponse.audio.done 之前的二进制 = AI 回复
- esponse.audio.done 之后的二进制 = 电话通话音频（可选）

**v0.3 P1 处理**：均送 opus_decode → playback_q → audio_out（不做区分）。
**P2 考虑**：电话通话音频可能需要单独编码（如 8kHz），届时通过 session.update 携带 sample_rate 字段。

### D.4 容错策略

`c
// proto_service 收到未知 type
void proto_on_unknown(cJSON *root) {
    const char *type = cJSON_GetObjectItem(root, type)->valuestring;
    LOG_W(proto, unknown frame type: %s ignored, type);
    metric.proto_unknown++;
    // 不主动断开连接，等待服务端继续推送已知帧
}

// 解析失败（如 JSON 语法错）
void proto_on_parse_error(const char *raw, size_t len) {
    LOG_E(proto, JSON parse failed len=%u dropping, len);
    metric.proto_parse_err++;
    // 单帧错误不触发重连；连续 10 次错误则断开重连
}
`

---

## §E 状态机 P1 完善（增强 v0.2 §7）

> 引用：v0.2 §7 状态机已含 P1 Listening⇄Playing。本节补充耳机插拔、ConfigAP P1、状态转移表。

### E.1 完整状态转移表（v0.3 增补）

| 当前状态 | 触发事件 | 目标状态 | 备注 |
|---------|---------|---------|------|
| BOOT | 自检通过 | INIT | 上电 |
| INIT | NVS 损坏 | CONFIG_AP | 红灯慢闪 + AP |
| INIT | Wi-Fi 关联失败 ≥ 3 次 | CONFIG_AP | 同上 |
| INIT | Wi-Fi OK + WS OK | SLEEP | 进入低功耗 |
| CONFIG_AP | 用户完成配网（HTTP POST）| INIT | 重启 |
| CONFIG_AP | 5 min 无操作 | SLEEP | 保留 NVS，AP 关闭 |
| SLEEP | 唤醒词命中 | LISTENING | 绿色快闪 3 次 |
| LISTENING | 收到下行音频首帧 | PLAYING | 切换 LED 慢闪 |
| LISTENING | 静音超时 / 平台 stop | SLEEP | 回到待机 |
| PLAYING | 收到下行 speech_started | LISTENING | Barge-In（≤ 50ms）|
| PLAYING | 收到 response.audio.done | LISTENING | AI 讲完 |
| PLAYING | 静音超时 | LISTENING | 平台未发 done 兜底 |
| SLEEP | OTA 触发（仅 SLEEP 允许）| OTA | 升级中 |
| LISTENING | OTA 触发 | (拒绝) | 协议层硬约束 |
| PLAYING | OTA 触发 | (拒绝) | 同上 |
| * | 致命错误 (E_WW_001 等)| FAULT | 红灯快闪 4Hz |
| FAULT | 5 min 自动恢复 | INIT | 重启尝试 |
| 任意 | 3.5mm 耳机插入/拔出 | (LED 切换) | 不切状态，仅切音频路由 |

### E.2 耳机插拔事件处理（P1 设备侧）

`c
// 耳机检测 GPIO 中断服务（Korvo-2 V3.1 GPIO 引脚待 OI-21 确认）
void IRAM_ATTR headphone_isr(void *arg) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    event_t ev = {
        .type = (gpio_get_level(HP_DETECT_GPIO) == 0) ? EVENT_HP_INSERT : EVENT_HP_REMOVE,
        .timestamp_us = esp_timer_get_time()
    };
    xQueueSendFromISR(q_event, &ev, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

void audio_route_handler(event_t ev) {
    static int64_t last_toggle_us = 0;
    int64_t now = esp_timer_get_time();
    // 软件消抖 200ms，避免插拔抖动
    if (now - last_toggle_us < 200000) return;
    last_toggle_us = now;
    
    switch (ev.type) {
        case EVENT_HP_INSERT:
            es8311_set_hp_output(true);     // 切换到耳机输出
            ns4150_enable(false);           // 关扬声器功放
            LOG_I(audio, headphone inserted);
            metric.hp_insert++;
            break;
        case EVENT_HP_REMOVE:
            es8311_set_hp_output(false);
            ns4150_enable(true);
            LOG_I(audio, headphone removed);
            metric.hp_remove++;
            break;
    }
    // 不切 Listening/Playing 状态
}
`

### E.3 ConfigAP 入口（P1 完整化）

`
CONFIG_AP 状态：
- 启动 AP（SSID: KorvoAgent-XXXX，最后 4 位 MAC）
- HTTP 服务端口 80，提供配网页面
  - GET  /                配网首页
  - POST /api/wifi/scan   扫描周边 AP
  - POST /api/wifi/set    保存 ssid + pass
  - POST /api/factory_reset 一键恢复出厂
- 5 min 无操作关闭 AP，回到 SLEEP（保留 NVS 配置）
- LED：橙色（绿+红交替）
`

> OI-21 待确认：耳机检测 GPIO 引脚 + ConfigAP 是否 P1 必做（v0.2 标 P1 候选 BLE 配网，Web 配网 v0.2 已实现）

---

## §F 新增 Open Issues 与里程碑

### F.1 OI-21~26（v0.3 新增）

| OI | 问题 | v0.3 决策 | 影响 |
|----|------|----------|------|
| **OI-21** | Korvo-2 V3.1 耳机检测引脚（PA 头）编号 + 中断配置？ | 硬件负责人确认 GPIO + 测试 | §E.2 耳机切换 |
| **OI-22** | tts_feedback 帧最终决策（fire-and-forget vs ACK）？ | v0.3：fire-and-forget | §C.4 |
| **OI-23** | session.update 下发的 VAD 参数，会话期内 vs 持久化策略？ | v0.3：会话期内优先 + 写 NVS | §B.4 |
| **OI-24** | 电话通话音频二进制帧（ACOS V1 §4 步骤 9）是否需要在 P1 区分？ | v0.3：P1 不区分，统一播放 | §D.3 |
| **OI-25** | 声音复刻（Web 控制台功能）是否下发到设备？ | v0.3：不下发，ACOS 服务端处理 | — |
| **OI-26** | 平台远程 OTA 触发时机（仅 SLEEP 还是其他）？ | v0.3：仅 SLEEP（v0.2 §4.11 约束延续）| — |

### F.2 里程碑 P1-4（v0.3 增补）

| Phase | 内容 | 周期 | 累计 |
|-------|------|------|------|
| **P1-1** | Opus 解码器 + ES8311/NS4150 + I²S1 播放通路 | 2 周 | 2 周 |
| **P1-2** | Barge-In 协调 + 播放队列 + 状态机扩展 | 1 周 | 3 周 |
| **P1-3** | 音量控制 + 设备切换 + 稳定性测试 | 1 周 | 4 周 |
| **P1-4（v0.3 新增）** | session.update 下发通道 + tts_feedback 上报 + mock server 联调 | 1 周 | 5 周 |

P1 总计 ~5 周（v0.2 估算 4 周，v0.3 +1 周用于扩展帧联调）。

---

## §G v0.3 风险新增

| 编号 | 风险 | P1 对策 | 关联章节 |
|------|------|---------|---------|
| **R-13** | session.update 协议服务端未实现，设备下发后无响应 | 设备 fire-and-forget，无 ACK 重试 | §B.2 |
| **R-14** | tts_feedback 上报频率不当导致带宽浪费 | P1 仅 start + done，不发 progress | §C.3 |
| **R-15** | 耳机插拔抖动（GPIO 反复触发）| 软件消抖 200ms + 中断锁定 500ms | §E.2 |
| **R-16** | ACOS 协议扩展（V2）正式定义后与 v0.3 扩展点冲突 | 解析器容错 + 字段忽略；版本号协商字段预留 | §D.4 |
| **R-17** | Barge-In mock 测试时延不稳定（mock server 注入）| 多次测量取 P95 | §A.3 |
| **R-18** | 设备 NVS 中 ad_threshold 是 float 类型，需确认 ESP-IDF nvs_set_float 支持 | 确认 ESP-IDF 版本 ≥ v5.0 | §B.3 |

---

## §H 待补充内容（v0.3 增补）

> 引用 v0.2 §15 并补充 P1 增项。

- [ ] FreeRTOS 任务优先级与栈大小的实测调优
- [ ] 与 ACOS mock server 的端到端协议联调报告（含 session.update / tts_feedback）
- [ ] Opus MOS 实测数据
- [ ] **Barge-In 时序实测（speech_started → audio_out stop 延迟 P95）**【v0.3 加 P95 要求】
- [ ] **session.update 下发→生效端到端延迟实测**【v0.3 新增】
- [ ] **tts_feedback 服务端识别率实测**【v0.3 新增】
- [ ] 72 h 压测的 heap 曲线
- [ ] 声音复刻音色在设备端的兼容性测试（P1 评估）
- [ ] OI-21 耳机检测引脚确认 + 中断服务稳定性测试

---

## §I 配套使用指引

### I.1 与 v0.2 主 TDD 配合

| 关注点 | 阅读顺序 |
|--------|---------|
| P0 全局设计 | v0.2 §1-§4 |
| P0 协议对齐 | v0.2 §6 + 本文档 §D.1 |
| P1 基础设计 | v0.2 §5 |
| **P1 强化设计** | **本文档 §A-§G** |
| 状态机 P1 | v0.2 §7 + 本文档 §E |
| 资源/构建 | v0.2 §9-§10 |
| 测试验收 | v0.2 §11 + 本文档 §H |
| OI 总览 | v0.2 §13 + 本文档 §F |

### I.2 冻结流程

1. **评审**：研发负责人 + 平台侧共同评审 v0.3
2. **合并**：v0.3 经评审通过后，合并入主 TDD，更新至 v1.0（单文档）
3. **基线**：v1.0 进入开发冻结，禁止改动协议层（仅 bug fix）

---

*本文档 v0.3 升级要点：Barge-In 时序、平台参数下发、播放状态反馈必做、协议扩展点、状态机 P1 完善。*
*配套主文档：技术设计文档-TDD.md v0.2（P0 + P1 基础设计）。*
*冻结后 v1.0 合并入主 TDD，进入开发阶段。*
