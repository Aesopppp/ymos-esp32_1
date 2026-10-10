# REST API v1.0（云管后端 → 微信小程序）

> **基础 URL**：`https://api.example.com/api/v1`
> **鉴权**：Header `Authorization: Bearer <jwt>`，JWT 由 `POST /auth/wx-login` 返回

## 通用约定

- 请求/响应均为 JSON
- 时间戳字段统一为 UNIX 秒（UTC）
- 错误响应统一格式：

```json
{
  "code": 40001,
  "message": "device not found",
  "request_id": "abc123"
}
```

| 错误码 | 含义 |
|----|----|
| 40001 | 资源不存在 |
| 40003 | 参数非法 |
| 40100 | 未登录 / token 过期 |
| 40300 | 无权限 |
| 40400 | 接口不存在 |
| 50000 | 服务器内部错误 |
| 50300 | MQTT broker 不可用 |

## 1. 鉴权

### POST `/auth/wx-login`

微信 `wx.login()` 拿到 code 后调用此接口换 JWT。

**请求**：
```json
{
  "code": "081abc123def456",
  "encryptedData": "xxx",   // 可选，用于解出手机号
  "iv": "xxx"
}
```

**响应**：
```json
{
  "token": "eyJhbGciOi...",
  "refresh_token": "rt_xxx",
  "expires_in": 7200,
  "user": {
    "user_id": "u_001",
    "nickname": "张工",
    "avatar": "https://..."
  }
}
```

### POST `/auth/refresh`

用 refresh_token 换新 token。

## 2. 设备

### GET `/devices`

列出当前用户绑定的所有设备。

**响应**：
```json
{
  "devices": [
    {
      "device_id": "korvo-001",
      "name": "工位A001",
      "room_id": "r_bj_01",
      "fw_version": "v1.0.0",
      "online": true,
      "last_seen_ts": 1719300000,
      "settings": {
        "volume": 60,
        "led_mode": "night",
        "room_id": "r_bj_01",
        "vad_enable": true,
        "barge_in_enable": true,
        "model": "korvo-v2"
      }
    }
  ]
}
```

### GET `/devices/{device_id}`

查单个设备详情（结构同上）。

### POST `/devices`

**管理员权限**。把新设备绑定到当前用户（产线已烧 mgmt_token）。

**请求**：
```json
{
  "device_id": "korvo-002",
  "mgmt_token": "sk-mgmt-...",
  "name": "工位A002",
  "room_id": "r_bj_01"
}
```

### DELETE `/devices/{device_id}`

解绑设备（不擦除设备端数据）。

### PATCH `/devices/{device_id}`

改设备名 / 房间。

**请求**：
```json
{
  "name": "工位A001（备）",
  "room_id": "r_bj_02"
}
```

### PUT `/devices/{device_id}/settings`

修改设备配置 → 触发 MQTT `config_update` 下发 → 等 5s 收设备 `config_report` → 返回新值。

**请求**：
```json
{
  "volume": 70,
  "led_mode": "off"
}
```

**响应**：
```json
{
  "applied": true,
  "settings": {"volume": 70, "led_mode": "off", ...}
}
```

**异常**：
- `50400` 设备 5s 未回执 `config_report`
- `40003` 字段值非法

### POST `/devices/{device_id}/command`

下发指令（reboot / reset_wifi / play_tts / factory_reset）。

**请求**：
```json
{
  "cmd": "reboot",
  "args": {}
}
```

**响应**：
```json
{
  "queued": true,
  "cmd_id": "cmd_xxx"
}
```

## 3. 房间

### GET `/rooms`

列出当前用户的所有房间。

### POST `/rooms` — 创建房间

```json
{ "name": "北京办公室", "parent_id": null }
```

### PUT `/rooms/{room_id}` — 改房间名

### DELETE `/rooms/{room_id}` — 删房间（设备 room_id 自动清空）

## 4. OTA

### GET `/firmwares`

列出平台所有固件版本（含发布状态）。

### POST `/firmwares` — 上传新固件（管理员）

**multipart/form-data**：
- `file`: .bin 文件
- `version`: `v1.2.3`
- `release_notes`: string
- `strategy`: `all` / `canary` / `beta`

**响应**：
```json
{
  "firmware_id": "fw_xxx",
  "version": "v1.2.3",
  "url": "https://ota.cos-mgmt.example.com/firmware/v1.2.3.bin",
  "sha256": "...",
  "size": 1234567
}
```

### POST `/firmwares/{firmware_id}/publish`

按 strategy 灰度推送到目标设备。

### GET `/devices/{device_id}/ota-history`

查该设备历史 OTA 记录。

```json
{
  "history": [
    {
      "version": "v1.0.0",
      "phase": "ok",
      "started_ts": 1719000000,
      "finished_ts": 1719000060
    }
  ]
}
```

## 5. 用户

### GET `/users/me`

返回当前用户信息。

### GET `/users/me/devices-summary`

Dashboard 用的统计：
```json
{
  "total": 10,
  "online": 8,
  "offline": 2,
  "fw_versions": {"v1.0.0": 8, "v0.9.5": 2}
}
```

---

*Last updated: 2026-09-24*
