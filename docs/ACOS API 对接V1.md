# ACOS API 对接文档 V1

> **版本**: v1.2  
> **更新日期**: 2026-06-27  
> **协议**: WebSocket (WSS)  
> **连接地址**: `wss://acos-platform.emicloud.com/acos-realtime`

---

## 1. 概述

ACOS Platform 提供**实时语音对话 API**。客户端只需：

1. 连接 WebSocket
2. 发送 **Opus 编码**的音频
3. 接收 **Opus 编码**的音频（二进制帧）

---

## 2. 鉴权方式

### 2.1 获取 API Key

联系运营获取，格式：`sk-{企业编码}-{32位随机字符串}`

### 2.2 连接时鉴权

连接后第一条消息必须是 `auth`：

```json
{
  "type": "auth",
  "token": "sk-baidu-a1b2c3d4e5f6g7h8i9j0k1l2m3n4o5p6q"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `type` | string | **是** | 消息类型，固定为 `"auth"` |
| `token` | string | **是** | API Key，格式：`sk-{企业编码}-{32位随机字符串}`。联系运营获取 |

鉴权成功服务端返回 `__proxy_connected__`，失败返回 `error` 并断开连接。

---

## 3. 音频格式

| | 输入（发送） | 输出（接收） |
|--|------------|------------|
| 编码 | **Opus** | **Opus**（60ms 一帧，码率 24kbps） |
| 采样率 | 16000 Hz | **16000 Hz**（服务端自动将 Qwen 原始 24kHz 降采样到 16kHz） |
| 声道 | 单声道 | 单声道 |
| 传输 | Base64 放在 `audio` 字段，带 `"encoding": "opus"` | **WebSocket 二进制帧**，原始 Opus 数据（无 JSON 包裹） |

### Opus 编码说明

发送 Opus 时需在消息中携带 `"encoding": "opus"`，服务端会自动解码为 PCM16 再转发给 AI 模型。

```json
{
  "type": "input_audio_buffer.append",
  "audio": "//uQxAAAAAANIAAAAAE...",
  "encoding": "opus"
}
```

**推荐参数：**
- 采样率：16000 Hz
- 帧长：20ms（320 样本/帧）
- 声道：单声道

> **注意**：输出为 Opus 编码的二进制帧（60ms），客户端收到后需用 Opus 解码器解码为 PCM16 再送 DAC 播放。

---

## 4. 对接流程

```
客户端                          服务端
  │                               │
  │──── 1. WebSocket 连接 ──────▶│
  │──── 2. auth (API Key) ──────▶│
  │◀── 3. __proxy_connected__ ───│  ← 鉴权通过
  │                               │
  │==== 开始对话 ================│
  │──── 4. 持续发送 Opus 音频 ──▶│  ← input_audio_buffer.append (文本帧)
  │◀── 5. speech_started ────────│  ← 文本帧 (JSON)
  │◀── 6. speech_stopped ────────│  ← 文本帧 (JSON)
  │◀── 7. 二进制 Opus 音频 ─────│  ← 二进制帧 (AI 回复音频，多个)
  │◀── 8. response.audio.done ───│  ← 文本帧 (JSON)
  │◀── 9. 二进制 Opus 音频 ─────│  ← 二进制帧 (电话通话音频，可选)
  │                               │
  │──── 重复 4-8 ────────────────│
  │──── 10. 断开连接 ───────────▶│
```

---

## 5. 客户端发送

### 5.1 发送音频（Opus 格式）

音频数据流：**MIC 采集 → PCM16 → Opus 编码 → Base64 → WebSocket JSON**

```
① MIC 采集：16kHz、单声道、16-bit PCM 音频数据
② Opus 编码：将 PCM16 帧（建议 20ms/320 样本）编码为 Opus 数据包
③ Base64：将 Opus 二进制数据编码为 Base64 字符串
④ 发送：组装 JSON 消息通过 WebSocket 发送
```

以 Opus 编码发送音频（适合 ESP32 等嵌入式设备，低带宽）：

```json
{
  "event_id": "event_1710000000000_abc123",
  "type": "input_audio_buffer.append",
  "audio": "//uQxAAAAAANIAAAAAE...",
  "encoding": "opus"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `event_id` | string | 否 | 消息唯一标识，用于日志追踪和调试。建议格式：`event_{时间戳}_{随机字符串}`。如果不传服务端会自动生成 |
| `type` | string | **是** | 消息类型，固定为 `"input_audio_buffer.append"` |
| `audio` | string | **是** | Base64 编码的 Opus 音频数据。处理流程：**MIC → PCM16 → Opus 编码 → Base64**。建议每帧 20ms（320 样本），持续发送不要停 |
| `encoding` | string | **是** | 音频编码格式，固定为 `"opus"`。服务端收到后会自动解码为 PCM16 再转发给 AI 模型 |

---

## 6. 服务端推送

服务端通过 **WebSocket 文本帧** 和 **WebSocket 二进制帧** 两种方式推送数据：

| 帧类型 | 识别方式 | 内容 | 处理方式 |
|--------|---------|------|---------|
| **文本帧** | `typeof data === 'string'` | JSON 控制消息 | `JSON.parse()` 解析后按 `type` 字段处理 |
| **二进制帧** | `data instanceof ArrayBuffer` | Opus 音频数据 | 直接送 Opus 解码器解码为 PCM16 播放 |

### 6.1 连接就绪 (文本帧)

```json
{ "type": "__proxy_connected__" }
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定为 `"__proxy_connected__"` |

收到此事件后说明鉴权通过、上游连接就绪，可以开始发送音频。

### 6.2 语音检测开始 (文本帧)

服务端在音频缓冲区中检测到语音开始时返回此事件。

**用途：实现语音打断（Voice Interruption）**。客户端收到此事件后，应立即**暂停/停止当前 AI 音频播放**，让用户可以直接说话打断 AI 的回复，实现自然对话体验。

```json
{
  "event_id": "event_Pvp8nEhsQuGCQbFJ9x58n",
  "type": "input_audio_buffer.speech_started",
  "audio_start_ms": 3647,
  "item_id": "item_YbAiGvK2H7YaS34o4R6Ba"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `event_id` | string | 本次事件唯一标识符 |
| `type` | string | 固定为 `"input_audio_buffer.speech_started"` |
| `audio_start_ms` | integer | 从音频开始写入缓冲区到首次检测到语音所经过的毫秒数 |
| `item_id` | string | 语音停止时将创建的用户消息项 ID |

**打断实现方式：**

收到 `speech_started` 时，客户端执行以下操作即可实现打断：

```javascript
case 'input_audio_buffer.speech_started':
  // 立即停止当前播放，实现打断
  audioPlayback.stop()          // 停止所有播放
  audioPlayback.clearQueue()    // 清空音频缓冲队列
  break
```

### 6.3 语音检测结束 (文本帧)

服务端在音频缓冲区中检测到语音结束时返回此事件。收到此事件后，可以准备接收下一轮 AI 回复。

```json
{
  "event_id": "event_UhQiqNVRsgUiq4KUS5Xb5",
  "type": "input_audio_buffer.speech_stopped",
  "audio_end_ms": 4453,
  "item_id": "item_YbAiGvK2H7YaS34o4R6Ba"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `event_id` | string | 本次事件唯一标识符 |
| `type` | string | 固定为 `"input_audio_buffer.speech_stopped"` |
| `audio_end_ms` | integer | 语音停止时刻距会话开始经过的毫秒数 |
| `item_id` | string | 已创建的用户消息项 ID |

### 6.4 AI 回复音频流 (二进制帧)

模型增量生成新的音频数据时以 **WebSocket 二进制帧** 发送。数据为 **Opus 编码**（60ms 一帧，码率 24kbps）。

**客户端处理流程：**

```
收到二进制帧 → Opus 解码器解码 → PCM16 (16kHz) → 送 DAC 播放
```

每帧参数：

| 参数 | 值 |
|------|-----|
| 编码 | Opus |
| 帧时长 | **60ms**（960 样本 @ 16kHz） |
| 码率 | 24 kbps |
| 原始 PCM16 大小 | 1920 字节 |
| Opus 编码后大小 | **约 100-300 字节** |

> **注意**：二进制帧不含任何标识头或元数据，收到后直接送 Opus 解码器即可。连续接收多个帧时，需按顺序解码并连续播放，避免帧间出现间隙。

### 6.5 AI 回复结束 (文本帧)

模型完成生成音频数据时返回此事件。收到此事件说明本轮 AI 回复结束，可以继续发送下一轮音频。

```json
{
  "event_id": "event_Le1TDl7VfyHQxl47DtGxI",
  "type": "response.audio.done",
  "response_id": "resp_HaVOPdbmX6vifiV5pAfJY",
  "item_id": "item_Ls6MtCUWO7LM4E59QziNv",
  "output_index": 0,
  "content_index": 0
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `event_id` | string | 本次事件唯一标识符 |
| `type` | string | 固定为 `"response.audio.done"` |
| `response_id` | string | 响应的唯一 ID |
| `item_id` | string | 消息项 ID |
| `output_index` | integer | 响应中输出项的索引，固定为 0 |
| `content_index` | integer | 内容部分的索引，固定为 0 |

### 6.6 电话通话音频流 (二进制帧)

电话通话模式下，服务端将对方语音实时转发给客户端。数据格式与 AI 回复音频完全相同——**WebSocket 二进制帧**，Opus 编码（60ms 一帧）。

**客户端处理流程：**

```
收到二进制帧 → Opus 解码器解码 → PCM16 (16kHz) → 送 DAC 播放
```

处理方式与 6.4 节完全相同，无需区分帧来源。

### 6.7 错误 (文本帧)

```json
{
  "type": "error",
  "error": { "message": "错误描述", "code": 1001 }
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | 固定为 `"error"` |
| `error.message` | string | 错误描述信息 |
| `error.code` | integer | 数字错误码，可用于程序化判断 |

**常见错误码：**

| 错误码 | 说明 |
|--------|------|
| `1001` | API Key 无效 |
| `1002` | API Key 已被禁用 |
| `1003` | API Key 已过期 |
| `1011` | 企业已被禁用 |
| `1012` | 企业已过期 |
| `2000` | 鉴权失败 |
| `2001` | 上游服务异常 |

错误发生后连接会被关闭，需要重新连接并鉴权。

---

## 7. 完整示例

```javascript
const API_KEY = 'sk-baidu-a1b2c3d4e5f6g7h8i9j0k1l2m3n4o2341';

// 1. 连接 WebSocket
const ws = new WebSocket('wss://acos-platform.emicloud.com/acos-realtime');
ws.binaryType = 'arraybuffer';  // 接收二进制帧
let recording = false;

ws.onopen = () => {
  // 2. 发送鉴权
  ws.send(JSON.stringify({ type: 'auth', token: API_KEY }));
};

ws.onmessage = (e) => {
  // 判断二进制帧还是文本帧
  if (e.data instanceof ArrayBuffer) {
    // === 二进制帧 → Opus 音频 ===
    playOpusAudio(e.data);
    return;
  }

  // === 文本帧 → JSON 控制消息 ===
  const msg = JSON.parse(e.data);

  switch (msg.type) {
    case '__proxy_connected__':
      console.log('已就绪，开始录音');
      startMic();
      break;

    case 'input_audio_buffer.speech_started':
      console.log('检测到说话 -> 打断 AI 播放');
      // 【打断实现】立即停止当前播放 + 清空缓冲
      stopCurrentAudio();
      break;

    case 'input_audio_buffer.speech_stopped':
      console.log('检测到静音');
      break;

    case 'response.audio.done':
      console.log('本轮回复结束');
      break;

    case 'error':
      console.error('错误:', msg.error?.message);
      break;
  }
};

// 3. Opus 音频播放
function playOpusAudio(arrayBuffer) {
  // 使用 WebCodecs API 解码 Opus
  if (!window.AudioDecoder) {
    console.warn('WebCodecs not supported');
    return;
  }

  const decoder = new AudioDecoder({
    output: (frame) => {
      // 将解码后的 PCM16 送 AudioContext 播放
      const sampleCount = frame.numberOfFrames;
      const buffer = new Float32Array(sampleCount);
      frame.copyTo(buffer, { planeIndex: 0, format: 'f32-planar' });
      frame.close();

      const ctx = audioCtx || new AudioContext();
      const audioBuffer = ctx.createBuffer(1, buffer.length, ctx.sampleRate);
      audioBuffer.getChannelData(0).set(buffer);
      const source = ctx.createBufferSource();
      source.buffer = audioBuffer;
      source.connect(ctx.destination);
      source.start();
    },
    error: (err) => console.error('Opus decode error:', err),
  });

  decoder.configure({
    codec: 'opus',
    sampleRate: 16000,
    numberOfChannels: 1,
  });

  const chunk = new EncodedAudioChunk({
    type: 'key',
    timestamp: 0,
    duration: 60_000,  // 60ms
    data: new Uint8Array(arrayBuffer),
  });

  decoder.decode(chunk);
}

// 4. 采集麦克风音频，编码为 Opus 发送
async function startMic() {
  const stream = await navigator.mediaDevices.getUserMedia({
    audio: { sampleRate: 16000, channelCount: 1 }
  });

  // 使用 Opus 编码器（如 opus-recorder 库）将 PCM 编码为 Opus 数据包
  const opusEncoder = new OpusEncoder({ sampleRate: 16000, channels: 1 });

  const ctx = new AudioContext({ sampleRate: 16000 });
  const src = ctx.createMediaStreamSource(stream);
  const proc = ctx.createScriptProcessor(2048, 1, 1);

  proc.onaudioprocess = (e) => {
    if (!recording) return;

    // PCM Float32 → Opus 编码
    const opusPacket = opusEncoder.encode(e.inputBuffer.getChannelData(0));

    ws.send(JSON.stringify({
      event_id: `event_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`,
      type: 'input_audio_buffer.append',
      audio: arrayBufferToBase64(opusPacket),
      encoding: 'opus'
    }));
  };

  src.connect(proc);
  proc.connect(ctx.destination);
  recording = true;
}

// 工具函数：ArrayBuffer → Base64
function arrayBufferToBase64(buffer) {
  const bytes = new Uint8Array(buffer);
  let binary = '';
  for (let i = 0; i < bytes.byteLength; i++) {
    binary += String.fromCharCode(bytes[i]);
  }
  return btoa(binary);
}

// 打断：收到 speech_started 时停止播放 + 清空队列
let currentSource = null;

function stopCurrentAudio() {
  if (currentSource) {
    try { currentSource.stop(); } catch (e) {}
    currentSource = null;
  }
  // 清空缓冲队列（如果有缓存的音频帧）
  audioBufferQueue = [];
}
```

> **ESP32 等嵌入式设备**：使用 ESP-ADF 或 ESP-IDF 的 Opus 编码器/解码器。接收时判断 WebSocket 帧类型（opcode）：`0x1` 为文本帧（JSON），`0x2` 为二进制帧（Opus 音频）。二进制帧直接送 Opus 解码器解码为 PCM16 后送 DAC 播放。

---

## 8. 常见问题

**Q: 连接后没收到 `__proxy_connected__`？**  
A: 检查 API Key 是否正确。

**Q: 发音频后没收到回复？**  
A: 确认音频格式为 Opus（带 `encoding: "opus"`），16kHz、单声道。

**Q: 如何区分收到的数据是文本还是二进制？**  
A: 浏览器端判断 `e.data instanceof ArrayBuffer`；Python 判断 `isinstance(message, bytes)`；ESP32 判断 WebSocket 帧的 opcode（`0x1`=文本，`0x2`=二进制）。

**Q: 二进制帧的 Opus 音频参数是什么？**  
A: 60ms 一帧，16kHz 采样率，单声道，码率 24kbps。收到后直接送 Opus 解码器解码为 PCM16 即可播放。

**Q: 音色/VAD 等参数怎么配？**  
A: 联系运营人员在后台配置模板中设置，客户端无需关心。

**Q: API Key 有有效期吗？**  
A: 有。API Key 创建时可以设置过期时间（`expires_at`），企业也可以设置过期时间（`expire_at`）。如果连接时返回 `EXPIRED_API_KEY` 或 `ENTERPRISE_EXPIRED`，说明 Key 或企业已过期，请联系运营续期。

**Q: 如何实现语音打断（边说边打断 AI）？**  
A: 语音打断依赖两个事件配合：
1. 收到 `input_audio_buffer.speech_started` → **立即停止当前 AI 音频播放，清空播放缓冲**，让用户直接说话
2. 收到 `input_audio_buffer.speech_stopped` → 用户说完，等待 AI 自动生成下一轮回复

核心逻辑只需两步：
```javascript
case 'input_audio_buffer.speech_started':
  stopCurrentAudio()    // 停止播放 + 清空缓冲
  break
```

服务端 VAD 检测到用户开始说话时，会自动发送 `response.cancel` 中断 AI 生成，同时下发 `speech_started` 通知客户端停止播放。无需额外操作。

---

> **技术支撑**：ACOS Platform 团队
