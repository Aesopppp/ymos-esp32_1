# MQTT 协议 v1.0

> **跨项目协议**：本文档由 `korvo_agent/`（ESP32 设备）、`cloud/korvo_mgmt/`（云管后端）、`wechat/korvo_miniapp/`（微信小程序）**三端共用**。
> 任何字段变更必须三端 PR 同步合并，禁止单端改动。

## 1. 通道与鉴权

| 项 | 值 |
|----|----|
| Broker | `mqtts://mgmt.example.com:8883` |
| Client ID | `korvo-{device_id}` |
| 设备鉴权 | `mgmt_token` （NVS key = `mgmt_token`，格式 `sk-mgmt-<32hex>-<serial>`） |
| Keepalive | 60s |
| 下行 QoS | 1 |
| 上行 QoS | 0 |
| Last Will | topic `dev/{device_id}/status` → payload `{"online":false}` |

## 2. Topic 命名

| 方向 | Topic | 谁发 | 谁收 |
|----|----|----|----|
| 下行 | `dev/{device_id}/cmd` | 云管平台 | 单台设备 |
| 上行 | `dev/{device_id}/telemetry` | 设备 | 云管平台 |
| 状态 | `dev/{device_id}/status` | broker Last Will | 云管平台 |

## 3. 消息帧（6 种）

所有消息均为 JSON 文本帧，`type` 字段必填，`ts` 为 UNIX 秒（UTC）。

### 3.1 云 → 设备（`dev/{device_id}/cmd`）

#### type=`auth_request`（可选 / 预留，本期不用）

设备上线后由云端主动校验 token。本期由 broker 层完成，不需要此消息。

#### type=`config_update` — 配置更新

```json
{
  "type": "config_update",
  "settings": {
    "volume": 60,
    "led_mode": "night",
    "room_id": "bedroom",
    "vad_enable": true,
    "barge_in_enable": true,
    "model": "korvo-v2"
  },
  "ts": 1719300000
}
```

**字段约束**：
- `settings` 内的字段**部分下发**即可，未列出的字段设备不动
- 设备收到后必须写 NVS + 广播 `EV_CONFIG_UPDATED` + 立即应用
- 应用成功后**必须回** `config_report`（§3.2 type=`config_report`），便于云端确认

| settings 字段 | 类型 | 合法值 | 默认值 | 应用模块 |
|----|----|----|----|----|
| `volume` | int | 0-100 | 50 | `audio_out` (ES8311 reg 0x07) |
| `led_mode` | string | `"normal"` / `"night"` / `"off"` | `"normal"` | `led_service` |
| `room_id` | string | ≤ 64 字符 | `""` | `cfg_service` (仅持久化) |
| `vad_enable` | bool | true / false | true | `audio_in` |
| `barge_in_enable` | bool | true / false | true | `audio_in` |
| `model` | string | ≤ 32 字符 | `""` | `cfg_service` (仅持久化) |

#### type=`command` — 指令下发

```json
{
  "type": "command",
  "cmd": "reboot",
  "args": {"text": "你好"},
  "ts": 1719300000
}
```

| `cmd` 枚举 | `args` 字段 | 行为 |
|----|----|----|
| `reboot` | 无 | 延迟 1s 后 `esp_restart()` |
| `reset_wifi` | 无 | 清除 wifi_ssid/pass, 触发重新配网 |
| `play_tts` | `text: string` | 用本地 TTS 播报（本期预留，未实现） |
| `factory_reset` | 无 | 擦除全部 NVS，重启进配网 |

**设备收到后必须**：日志记录 + 立即执行。**无需上行回复**。

#### type=`ota` — OTA 推送

```json
{
  "type": "ota",
  "version": "v1.2.3",
  "url": "https://ota.example.com/firmware/v1.2.3.bin",
  "sha256": "a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6",
  "size": 1234567,
  "force": false,
  "ts": 1719300000
}
```

| 字段 | 类型 | 必填 | 说明 |
|----|----|----|----|
| `version` | string | ✓ | 设备必须比对，避免重复升级 |
| `url` | string | ✓ | HTTPS 公网可下载 |
| `sha256` | string | ✓ | 64 hex chars，下载后校验 |
| `size` | int | ✓ | 字节数，超出 OTA 分区拒绝 |
| `force` | bool | × | true=忽略电量/通话状态强制升级 |

### 3.2 设备 → 云（`dev/{device_id}/telemetry`）

#### type=`auth` — 鉴权（首次连接发送一次）

```json
{
  "type": "auth",
  "mgmt_token": "sk-mgmt-a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6-k001",
  "device_id": "korvo-001",
  "fw_version": "v1.0.0"
}
```

**注意**：本期由 broker 连接层做鉴权（EMQX 的 username/password），此消息**仅用于业务层记录**，不强制发送。

#### type=`config_report` — 配置状态上报

```json
{
  "type": "config_report",
  "settings": {
    "volume": 60,
    "led_mode": "night",
    "room_id": "bedroom",
    "vad_enable": true,
    "barge_model": true,
    "model": "korvo-v2"
  },
  "reason": "boot",
  "ts": 1719300000
}
```

| `reason` 枚举 | 触发时机 |
|----|----|
| `boot` | 设备启动加载完 user settings 后 |
| `cloud_update` | 收到云端 `config_update` 应用成功后回执 |
| `local_change` | 用户在设备本地按键/串口修改了配置（本期不实现） |

#### type=`ota_progress` — OTA 进度与结果

```json
{
  "type": "ota_progress",
  "phase": "downloading",
  "percent": 45,
  "error": "",
  "ts": 1719300000
}
```

| `phase` 枚举 | 含义 | `percent` |
|----|----|----|
| `downloading` | HTTPS 下载中 | 0-100 |
| `verifying` | SHA256 校验中 | 100 |
| `applying` | 写 OTA 分区中 | 100 |
| `rebooting` | 已调 `esp_restart()` | 100 |
| `ok` | 升级成功 | 100 |
| `failed` | 失败 | -1 |

**`failed` 时必须填 `error`**，取值：

| error | 含义 |
|----|----|
| `sha256_mismatch` | 校验失败 |
| `flash_write_error` | 写 OTA 分区失败 |
| `partition_too_small` | size 超过 OTA 分区 |
| `low_battery` | 电量 < 30% 且非 force |
| `in_call` | 正在通话中且非 force |
| `download_timeout` | HTTPS 下载超时 |
| `http_error` | HTTP 状态码非 200 |

## 4. 错误码

| 码 | 含义 |
|----|----|
| `0` | 成功 |
| `1` | 协议解析失败（JSON 格式错误） |
| `2` | 必填字段缺失 |
| `3` | settings 字段值非法（如 volume=200） |
| `4` | 设备忙（OTA 中收到 config_update 忽略） |
| `5` | 内部错误 |

## 5. 版本与兼容性

- **当前版本**：v1.0
- **向后兼容原则**：
  - 设备**必须**忽略未知字段（forward-compat）
  - 云端**必须**忽略设备未上报的字段（backward-compat）
  - 字段**删除**需要升 v2.0，**新增字段**升 v1.x

## 6. 参考实现

- 设备侧：`korvo_agent/main/proto_mgmt.c` （基于 `proto_service.c` 扩展）
- 云管侧：`cloud/korvo_mgmt/internal/mqtt/handler.go`
- 测试工具：`docs/protocol/test-client/`（待补）

---

*Last updated: 2026-09-24 — created alongside PRD-智能语音辅助设备.md §11I.3*
