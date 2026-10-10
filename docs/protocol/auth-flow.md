# 鉴权流程 v1.1

> **跨项目协议**：本文档由 `korvo_agent/`（ESP32 设备）、`cloud/korvo_mgmt/`（云管后端）、`wechat/korvo_miniapp/`（微信小程序）、`cloud/korvo_mgmt/cmd/admin-web/`（管理后台）**四端共用**。
> 任何字段变更必须四端 PR 同步合并，禁止单端改动。
>
> **版本**：v1.1（2026-09-24 更新 mgmt_token 格式 + 新增管理员 API Key）

## 1. 鉴权凭证总览

| 类型 | 用途 | 格式 | 谁持有 | 创建方 |
|----|----|----|----|----|
| **mgmt_token** | 设备 → 云管 MQTT 鉴权 | `sk-{device_code}-{32随机}` | 烧在设备 NVS 里 | 产线注册时云管生成 |
| **管理员 API Key** | 管理员 → 云管 REST API | `sk-adm-{32随机}` | 管理员/运维人员 | 管理后台创建 |
| **小程序 JWT** | 终端用户 → 云管 REST API | JWT (HS256) | 微信小程序 | `wx.login` 换 |

## 2. mgmt_token 颁发（产线环节）

```
┌─────────┐  POST /api/v1/devices    ┌──────────┐
│ 产线工具  │ ──────────────────────→ │ 云管后端   │
│ factory_burn│                        │          │
└─────────┘    ←───────────────────── └──────────┘
              {device_id, mgmt_token}
```

1. 产线工具调 `POST /api/v1/devices` 注册新设备（**管理员 API Key 鉴权**）
2. 云管后端生成 `mgmt_token`（见 §4），INSERT 到 `device` 表，返回给产线工具
3. 产线工具调 `tools/factory_burn.py` 把 `mgmt_token` 烧入设备 NVS `mgmt_token` key
4. **注意**：此步需要管理员 API Key，生产环境建议限 IP 白名单

## 3. MQTT 鉴权（设备运行时）

```
┌──────────┐  CONNECT (client_id=korvo-{device_id},    ┌──────────┐
│ ESP32 设备│    username=mgmt_token, password=x) ───→ │ EMQX     │
└──────────┘                                            │          │
              ← CONNACK (rc=0 / rc=5) ────────────────  └──────────┘
```

- **EMQX 配置**：使用 `password_based` 认证插件
- **username** = `mgmt_token`（值即 token 字符串）
- **password** = 任意值（本期不用，由 token 唯一鉴权）
- 鉴权失败（rc=5）→ 设备断连 → 指数退避重连

## 4. mgmt_token 格式与生成（v1.1 更新）

### 4.1 格式

```
sk-{device_code}-{32随机}
```

| 部分 | 含义 | 示例 |
|----|----|----|
| `sk-` | 固定前缀（secret key） | `sk-` |
| `{device_code}` | 设备序列号 / 工厂编号 | `K001`、`SN-2025-001` |
| `{32随机}` | 32 字符 base62 随机串 | `7n3k9x2v8q5w1r2t3y4u5i6o7p8a9s0d` |

**完整示例**：`sk-K001-7n3k9x2v8q5w1r2t3y4u5i6o7p8a9s0d`

**字符集**：base62 = `[0-9A-Za-z]`，共 62^32 ≈ 2.27×10^57 组合，远超安全强度。

### 4.2 生成算法（Go）

```go
import (
    "crypto/rand"
    "math/big"
)

const base62Chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

func GenerateMgmtToken(deviceCode string) (string, error) {
    var buf [32]byte
    for i := 0; i < 32; i++ {
        n, err := rand.Int(rand.Reader, big.NewInt(int64(len(base62Chars))))
        if err != nil {
            return "", err
        }
        buf[i] = base62Chars[n.Int64()]
    }
    return fmt.Sprintf("sk-%s-%s", deviceCode, string(buf[:])), nil
}
```

### 4.3 校验算法

```go
// 服务端解析 token
func ParseMgmtToken(token string) (deviceCode string, rand32 string, err error) {
    parts := strings.Split(token, "-")
    if len(parts) != 3 || parts[0] != "sk" {
        return "", "", errors.New("invalid token format")
    }
    if len(parts[2]) != 32 {
        return "", "", errors.New("invalid random part length")
    }
    // 校验 base62 字符集
    for _, c := range parts[2] {
        if !strings.ContainsRune(base62Chars, c) {
            return "", "", errors.New("invalid character")
        }
    }
    return parts[1], parts[2], nil
}
```

### 4.4 存储

- **设备端**：NVS 明文（key=`mgmt_token`，namespace=`korvo_cfg`）
- **云管端**：**AES-256-GCM 加密**（key 从 Vault 读）
  - DB 列：`device.mgmt_token_enc` (BYTEA)
  - EMQX auth 插件查时先解密

### 4.5 重新生成

- 管理后台 → 设备详情 → "重新生成 mgmt_token" 按钮
- 生成新 token 后**旧 token 立即失效**（DB 覆盖）
- 产线需重新烧录设备
- 记录到 `admin_audit_log`

## 5. 管理员 API Key（新功能 · v1.1）

### 5.1 格式

```
sk-adm-{32随机}
```

**示例**：`sk-adm-7n3k9x2v8q5w1r2t3y4u5i6o7p8a9s0d`

> 与 mgmt_token 区别：前缀 `adm` 表示 admin（管理员），device_code 部分**省略**（因为管理员 key 不绑定单台设备）。

### 5.2 权限模型（RBAC）

每个 API Key 创建时授予一个**权限范围**（scope），由多个 `资源:动作` 组成：

| Scope | 含义 |
|----|----|
| `device:read` | 查看设备列表 / 详情 / 历史 |
| `device:write` | 修改设备配置 / 解绑 / 重启 |
| `device:delete` | 删除设备记录 |
| `firmware:read` | 查看固件列表 / OTA 进度 |
| `firmware:write` | 上传固件 / 触发发布 |
| `user:read` | 查看用户列表 / 详情 |
| `user:write` | 修改用户信息 / 封禁 |
| `admin:all` | 超级管理员（绕过所有检查） |

**示例**：
```json
{
  "key_id": "sk-adm-7n3k...",
  "name": "ops-daily",
  "scopes": ["device:read", "device:write", "firmware:read"],
  "created_by": "admin@korvo.cloud",
  "created_at": 1719300000,
  "expires_at": 1721980800
}
```

### 5.3 颁发流程

```
[管理员] → 管理后台 → [+ 创建 API Key]
                       │
                       ├─ 填名称、勾选 scopes、选过期
                       │
                       ↓
                   POST /admin/api-keys (admin JWT)
                       │
                       ↓
                   服务端生成 sk-adm-{32随机}
                   INSERT api_key 表
                   返回完整 key（仅此一次可见）
                       │
                       ↓
                   弹窗显示并强制用户复制
```

**重要**：完整 key 仅在**创建时**返回一次，之后只能看到 `key_id`（前 8 位 + `...`）。

### 5.4 调用方式

```http
GET /admin/devices HTTP/1.1
Host: api.example.com
Authorization: Bearer sk-adm-7n3k9x2v8q5w1r2t3y4u5i6o7p8a9s0d
X-Request-ID: abc123
```

**服务端校验**：
1. 解析 `Authorization: Bearer <token>`
2. 查 `api_key` 表（按 `key_id` 前 32 字符 + AES 解密比对）
3. 校验 `expires_at` 未过期
4. 校验 `revoked_at IS NULL`
5. 校验请求路径匹配 `scopes`

### 4.5 存储

| 字段 | 类型 | 说明 |
|----|----|----|
| `key_id` | VARCHAR(48) | PK，完整 `sk-adm-{32随机}`（不再哈希，因为要按原文查） |
| `name` | VARCHAR(64) | 用户起的名字 |
| `scopes` | TEXT[] | 权限数组 |
| `created_by` | VARCHAR(32) | 创建者（admin user_id） |
| `created_at` | BIGINT | |
| `expires_at` | BIGINT | NULL=永不过期 |
| `revoked_at` | BIGINT | NULL=未吊销 |
| `last_used_at` | BIGINT | 用于审计 + UI 显示 |
| `last_used_ip` | VARCHAR(45) | IPv4/IPv6 |

> **为什么不哈希存？** 因为 admin 调用量小（< 1万/天），DB 直接全表扫性能足够；且 admin key 需要支持"按原文查"才能精确比对。哈希存会让"重新生成"功能复杂化。

## 6. 小程序 JWT 鉴权（不变）

```
┌──────────┐  wx.login() -> code          ┌──────────┐
│ 微信小程序│ ─────────────────────────────→│ 微信服务器 │
└──────────┘                                └──────────┘
              ← openid + session_key
┌──────────┐  POST /auth/wx-login {code}   ┌──────────┐
│ 小程序    │ ─────────────────────────────→ │ 云管后端   │
└──────────┘                                │          │
              ← {token, refresh_token, user}└──────────┘
```

- JWT payload: `{ user_id, openid, exp }`
- 默认 `expires_in = 7200`（2 小时）
- `refresh_token` 有效期 30 天

## 7. 权限矩阵（更新）

| 角色 / 凭证 | mgmt_token | 管理员 API Key | wx.login JWT |
|----|----|----|----|
| 设备连 MQTT | ✓ | - | - |
| 产线烧 token | - | ✓（需 `device:write`） | - |
| 用户绑定设备 | - | ✓（需 `user:write`） | ✓（仅本人） |
| 用户读配置 / 改配置 / OTA | - | - | ✓（仅本人设备） |
| 上传新固件 | - | ✓（需 `firmware:write`） | - |
| 推送 OTA | - | ✓（需 `firmware:write`） | - |
| 管理后台登录 | - | - | -（用管理员账号密码 + MFA） |

 > **管理后台登录**本身**不**用 API Key，而是用**管理员账号密码 + TOTP**（详见 `PRD-cloud-platform.md` §7.6.1）。

## 8. 安全注意事项

1. **不要**在任何日志里打印完整 token
2. **不要**在 URL 里传 token（用 Header）
3. 小程序 JWT + 管理员 API Key 都必须 HTTPS
4. 云管后端的 `MGMT_TOKEN_SECRET` / `DB_FIELD_KEY` / `ADMIN_JWT_SECRET` 必须从 Vault 读，不要 commit
5. EMQX 启用 TLS 1.2+，禁用 1883 明文端口
6. 管理员 API Key 默认 90 天过期，强制续期
7. 关键操作（重新生成 mgmt_token、吊销 API Key）记 `admin_audit_log`

---

*Last updated: 2026-09-24 — v1.1 更新 mgmt_token 格式 + 新增管理员 API Key*
*Previous: v1.0 (2026-09-24 初始)*
