# PRD：智能语音辅助设备云管理平台

> **配套文档**：
> - 设备侧 PRD：`../project/korvo_agent/PRD-智能语音辅助设备.md`（含 §11I 协议）
> - 协议规范：`../docs/protocol/mqtt-v1.0.md`、`rest-api-v1.0.md`、`auth-flow.md`
> - 工程 README：`../cloud/korvo_mgmt/README.md`
> - 管理后台 UI/UX：见本 PRD §7

## 1. 产品概述

### 1.1 背景与目标

当前设备只通过 WebSocket 连 ACOS AI 平台，没有独立的设备管理通道。本平台为以下需求提供后端能力：

1. 给设备提供 MQTT 通道（接收 `config_report` / `ota_progress` 上行）
2. 给微信小程序提供 REST API（看设备、改设置、推送 OTA）
3. OTA 固件包的存储与分发（OSS + 签名）
4. 设备/用户/房间的元数据管理

### 1.2 产品定位

| 维度 | 本平台 | ACOS AI 平台 |
|----|----|----|
| 定位 | 设备**管理** + 用户控制 | AI **推理** + 通话 |
| 协议 | MQTT + REST | WebSocket + JSON |
| 鉴权 | mgmt_token / JWT | acos_token |
| 数据 | 配置元数据、固件包、用户 | 音频流、AI 模型 |
| 实时性要求 | 中（OTA 异步下发） | 高（音频低延迟） |

> **两个平台职责完全独立**：本平台不知道 ACOS 的内部协议，ACOS 也不知道本平台存在。

### 1.3 关键约束（P0 范围）

- 仅支持 ESP32-S3 设备（型号固定）
- 单租户（不分代理商 / 子账号，P2 再加）
- 中文用户界面（先国内用）
- 支持 1000 台设备在线（MQTT 单 broker 即可），10000+ 需要 K8s 多 broker 联邦
- 日志保留 30 天

### 1.4 产品路线图

| 期 | 范围 |
|----|----|
| **P0** | 设备注册、MQTT 鉴权、config_update 单台、OTA 全量推送 |
| **P1** | 灰度策略（按 device_id 段）、OTA 进度聚合、Dashboard、批量配置 |
| **P2** | 多租户、SSO、固件差分升级、设备分组权限 |

## 2. 系统架构

### 2.1 整体架构

```
┌──────────────────────────────────────────────────────────────────────────┐
│                         生产环境（K8s 集群）                              │
│                                                                          │
│ ┌──────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────────┐   │
│ │ REST API │   │ MQTT Bridge  │   │   Postgres   │   │    Redis     │   │
│ │  (Gin)   │   │  (Eclipse    │   │     16       │   │     7        │   │
│ │          │   │   Paho)      │   │  (RDS 托管)  │   │  (Cluster)   │   │
│ └────┬─────┘   └──────┬───────┘   └──────┬───────┘   └──────┬───────┘   │
│      │                │                  │                  │            │
│      └────────────────┴──────────────────┴──────────────────┘            │
│                              │                                           │
│                       ┌──────┴──────┐                                    │
│                       │   EMQX 5.x  │  (StatefulSet, 3 副本)             │
│                       │  (Broker)   │                                    │
│                       └──────┬──────┘                                    │
└──────────────────────────────┼───────────────────────────────────────────┘
                               │
                          mqtts://8883
                               │
                       ┌───────┴───────┐
                       │  ESP32 设备   │
                       │  (korvo_agent)│
                       └───────────────┘
                               ↑
                          HTTPS REST
                               │
                       ┌───────┴───────┐
                       │  微信小程序   │
                       └───────────────┘
```

### 2.2 技术栈

| 层 | 选型 | 理由 |
|----|----|----|
| 语言 | Go 1.22+ | 高并发 MQTT 客户端（goroutine）、单文件部署、调试方便 |
| HTTP 框架 | Gin 1.10 | 性能 / 生态 |
| MQTT broker | EMQX 5.6（开源版） | 原生 Dashboard、REST API、auth 插件支持自定义 |
| MQTT 客户端 | Eclipse Paho | Go 最稳定的 MQTT 客户端 |
| 数据库 | PostgreSQL 16 | 强类型 + JSONB（存配置 settings）+ 全文检索 |
| ORM | sqlc | 用 SQL 生成 Go 代码，比 GORM 更类型安全 |
| 缓存 | Redis 7 | 限流、session、JWT 黑名单 |
| 对象存储 | 阿里云 OSS | 存固件包 + 走 CDN 加速下载 |
| 配置中心 | 环境变量（开发）/ Vault（生产） | 不 commit 密钥 |
| 监控 | Prometheus + Grafana | 标准云原生方案 |
| 日志 | Loki + Promtail | 与 Prometheus 联动 |
| 部署 | Docker Compose（开发）/ K8s（生产） | |

## 3. 数据模型

### 3.1 ER 关系

```
user ───< user_device >─── device
                            │
                            ├──────< ota_log >── firmware
                            │
                            └──< config_change_log

user ───< user_room >─── room
                         │
                         └──< device (room_id)
```

### 3.2 表结构

#### `user` 表

| 字段 | 类型 | 约束 | 说明 |
|----|----|----|----|
| `user_id` | VARCHAR(32) | PK | `u_<uuid>` |
| `wx_openid` | VARCHAR(64) | UNIQUE NOT NULL | 微信 openid |
| `wx_unionid` | VARCHAR(64) | NULL | 多小程序互通 |
| `nickname` | VARCHAR(64) | | 微信昵称 |
| `avatar_url` | TEXT | | 头像 |
| `phone` | VARCHAR(20) | NULL | 解密后的手机号 |
| `role` | SMALLINT | DEFAULT 0 | 0=普通用户 1=管理员 |
| `created_at` | TIMESTAMPTZ | DEFAULT now() | |
| `updated_at` | TIMESTAMPTZ | DEFAULT now() | |

#### `room` 表

| 字段 | 类型 | 约束 | 说明 |
|----|----|----|----|
| `room_id` | VARCHAR(32) | PK | `r_<uuid>` |
| `owner_user_id` | VARCHAR(32) | FK→user | 创建者 |
| `name` | VARCHAR(64) | NOT NULL | 房间名 |
| `parent_id` | VARCHAR(32) | NULL FK→room | 支持嵌套（如"总部/3 楼/A 区") |
| `created_at` | TIMESTAMPTZ | | |

#### `device` 表

| 字段 | 类型 | 约束 | 说明 |
|----|----|----|----|
| `device_id` | VARCHAR(32) | PK | `korvo-<serial>` |
| `serial` | VARCHAR(32) | UNIQUE NOT NULL | 工厂序列号 |
| `mgmt_token_enc` | BYTEA | NOT NULL | AES-256-GCM 加密后的 mgmt_token |
| `acos_token_enc` | BYTEA | NULL | 同上（可选存储） |
| `fw_version` | VARCHAR(16) | DEFAULT '' | 当前固件版本 |
| `name` | VARCHAR(64) | | 用户改的设备名 |
| `room_id` | VARCHAR(32) | NULL FK→room | |
| `online` | BOOLEAN | DEFAULT false | Last Will 上报 |
| `last_seen_ts` | BIGINT | | UNIX 秒 |
| `settings` | JSONB | DEFAULT '{}' | 配置缓存 `{volume:60,led_mode:chr(34)nightchr(34),...}` |
| `created_at` | TIMESTAMPTZ | | |

> **加密字段**：用 AES-256-GCM，主密钥从 Vault 读，仅服务进程可解密。

#### `user_device` 表（多对多：用户绑定设备）

| 字段 | 类型 | 约束 |
|----|----|----|
| `user_id` | VARCHAR(32) | PK, FK→user |
| `device_id` | VARCHAR(32) | PK, FK→device |
| `role` | SMALLINT | 0=普通 1=管理员（家庭场景主账号） |
| `created_at` | TIMESTAMPTZ | | |

#### `admin_user` 表（管理后台账号）

| 字段 | 类型 | 说明 |
|----|----|----|----|
| `admin_id` | VARCHAR(32) PK | |
| `email` | VARCHAR(64) UNIQUE | 登录用 |
| `password_hash` | VARCHAR(128) | bcrypt cost=12 |
| `password_changed_at` | BIGINT | 90 天强制改 |
| `mfa_secret_enc` | BYTEA | TOTP secret（AES 加密） |
| `mfa_bound_at` | BIGINT | 何时绑定 |
| `role` | SMALLINT | 0=普通 1=super |
| `last_login_at` | BIGINT | |
| `locked_until` | BIGINT | 错误次数过多锁定 |
| `created_at` | TIMESTAMPTZ | |

#### `api_key` 表（管理员 API Key）

| 字段 | 类型 | 说明 |
|----|----|----|----|
| `key_id` | VARCHAR(48) PK | 完整 `sk-adm-{32随机}` |
| `name` | VARCHAR(64) | 用户起的名字 |
| `scopes` | TEXT[] | 权限数组（如 `{device:read,firmware:write}`） |
| `created_by` | VARCHAR(32) FK→admin_user | |
| `created_at` | BIGINT | |
| `expires_at` | BIGINT | NULL=永不过期 |
| `revoked_at` | BIGINT | NULL=未吊销 |
| `revoked_by` | VARCHAR(32) | |
| `last_used_at` | BIGINT | 用于审计 + UI 显示 |
| `last_used_ip` | VARCHAR(45) | IPv4/IPv6 |

#### `admin_audit_log` 表（审计日志）

| 字段 | 类型 | 说明 |
|----|----|----|----|
| `log_id` | BIGSERIAL PK | |
| `admin_id` | VARCHAR(32) FK | NULL=用 API Key 操作 |
| `api_key_id` | VARCHAR(48) FK | NULL=用 JWT 操作 |
| `action` | VARCHAR(64) | `device.reboot` / `api_key.create` 等 |
| `resource_type` | VARCHAR(32) | |
| `resource_id` | VARCHAR(64) | |
| `before` | JSONB | |
| `after` | JSONB | |
| `ip` | VARCHAR(45) | 操作者 IP |
| `user_agent` | TEXT | |
| `ts` | BIGINT | |

> **保留 1 年**，定时任务清理。

#### `firmware` 表

| 字段 | 类型 | 约束 | 说明 |
|----|----|----|----|
| `firmware_id` | VARCHAR(32) | PK | `fw_<uuid>` |
| `version` | VARCHAR(16) | UNIQUE NOT NULL | `v1.2.3` |
| `oss_url` | TEXT | NOT NULL | 公网下载 URL |
| `sha256` | VARCHAR(64) | NOT NULL | 64 hex |
| `size` | BIGINT | NOT NULL | 字节数 |
| `release_notes` | TEXT | | 发版说明 |
| `strategy` | SMALLINT | | 0=all 1=canary 2=beta |
| `strategy_meta` | JSONB | DEFAULT '{}' | `{canary_percent: 5, beta_user_ids: [...]}` |
| `status` | SMALLINT | | 0=draft 1=published 2=paused 3=archived |
| `created_at` | TIMESTAMPTZ | | |
| `published_at` | TIMESTAMPTZ | NULL | |

#### `ota_log` 表

| 字段 | 类型 | 约束 | 说明 |
|----|----|----|----|
| `log_id` | VARCHAR(32) | PK | `otalog_<uuid>` |
| `device_id` | VARCHAR(32) | FK→device | |
| `firmware_id` | VARCHAR(32) | FK→firmware | |
| `phase` | VARCHAR(16) | | downloading/verifying/applying/rebooting/ok/failed |
| `percent` | INT | | 0-100, -1=失败 |
| `error` | VARCHAR(64) | NULL | 失败原因（见 mqtt-v1.0.md §3.2） |
| `started_ts` | BIGINT | | MQTT 收到第一帧 |
| `finished_ts` | BIGINT | NULL | ok 或 failed 时 |
| `created_at` | TIMESTAMPTZ | DEFAULT now() | |

#### `config_change_log` 表（审计）

| 字段 | 类型 | 约束 |
|----|----|----|
| `log_id` | BIGSERIAL | PK |
| `device_id` | VARCHAR(32) | FK |
| `changed_by` | VARCHAR(32) | 'cloud' / 'local' / 'user_<id>' |
| `before` | JSONB | |
| `after` | JSONB | |
| `ts` | BIGINT | |

> **30 天后自动清理**：定时任务 `DELETE FROM config_change_log WHERE created_at < now() - interval '30 days'`。

### 3.3 索引

```sql
CREATE INDEX idx_device_room ON device(room_id);
CREATE INDEX idx_device_online ON device(online);
CREATE INDEX idx_ota_log_device_ts ON ota_log(device_id, started_ts DESC);
CREATE INDEX idx_user_device_user ON user_device(user_id);
CREATE INDEX idx_config_log_device_ts ON config_change_log(device_id, ts DESC);
```

## 4. 核心流程

### 4.1 设备注册 + mgmt_token 颁发

```
┌─────────┐  POST /api/v1/devices   ┌─────────┐  INSERT device,   ┌─────────┐
│ 产线工具 │ {serial} (admin JWT) ──→│ REST API│  生成 mgmt_token  │Postgres │
│         │                          │         │─────────────────→│         │
│         │                          │         │  返回 mgmt_token  │         │
│         │  ←───────────────────────│         │←────────────────│         │
└─────────┘  {device_id, mgmt_token} └─────────┘                  └─────────┘
       │                                │
       │ 调用 factory_burn.py            │
       ↓                                │
   烧入设备 NVS                          │
       │                                │
       │                                ↓
       │                          将 token 加入
       │                          EMQX 黑/白名单
       ↓                                │
   设备首次开机                       ↓
   MQTT CONNECT                      EMQX auth 插件
   username=mgmt_token               校验 mgmt_token
   ↓                                 ↓
   ✅ 上线
```

**关键点**：
- mgmt_token 生成算法见 `auth-flow.md §4`
- EMQX auth 插件从 `device` 表读所有 `mgmt_token_enc`，解密后比对 username
- 性能优化：EMQX 启动时全量加载到 Redis（设备数 ≤1万 不需要）

### 4.2 MQTT 鉴权

EMQX `password_based` 认证 + 自定义 HTTP auth 插件：

```
[ESP32] → CONNECT {client_id, username=mgmt_token}
   ↓
[EMQX] → POST http://korvo_api:8080/internal/mqtt/auth {username, client_id}
   ↓
[API] → 解密 token→device_id → 校验存在 → 返回 200
   ↓
[EMQX] → CONNACK rc=0 / rc=5
```

**为什么用 HTTP 而不直接读 DB？**
- 加密逻辑集中在一处（API），EMQX 不接触密钥
- 设备禁用时立即生效（只需缓存 invalidate）

**缓存**：Redis 缓存 `(mgmt_token_hash → device_id)` 5 分钟，命中率 >99%。

### 4.3 config_update 下发（核心）

```
┌─────────┐  PUT /devices/{id}/settings  ┌─────────┐
│ 小程序   │  {volume: 70}  ──────────→ │ REST API│
└─────────┘                              │         │
                            │             │  1. 鉴权
                            │             │  2. 校验字段合法性
                            │             │  3. UPDATE device.settings
                            │             │  4. INSERT config_change_log
                            │             │  5. PUBLISH MQTT config_update
                            │             │     topic=dev/{id}/cmd
                            │             │     QoS=1
                            │             │  6. 等 5s 收 config_report
                            │             │  7. 返回 applied=true/false
                            │             ↓
                            │           ┌─────────┐
                            │           │ EMQX    │
                            │           │ 转发    │
                            │           └────┬────┘
                            │                ↓
                            │           ┌─────────┐
                            │           │ ESP32   │
                            │           │  收到   │
                            │           │ 应用    │
                            │           │ 回执   │
                            │           └────┬────┘
                            │                ↓
                            └─────── 收 config_report ──┐
                                                       │
                                                       ↓
                                                  标记 applied=true
```

**失败处理**：
- 5s 没收到 `config_report`：返回 `50400`，让小程序展示"设备未响应"
- 收到 `config_report` 但值不等：返回 `200` 但 `applied=false`，附实际值

### 4.4 OTA 灰度推送

**策略类型**：

| `strategy` | 行为 |
|----|----|
| `all` (0) | 所有设备立即推送 |
| `canary` (1) | 按 `canary_percent` 随机选 N% 推送（如 5%） |
| `beta` (2) | 推送到 `beta_user_ids` 名单里的用户所绑定的设备 |

**流程**：

```
[管理员] → POST /firmwares (上传 .bin)
                    ↓
              存 OSS 拿 url
              INSERT firmware(status=draft)
                    ↓
[管理员] → POST /firmwares/{id}/publish
                    ↓
              根据 strategy 选出 target_device_ids:
                - all:     SELECT device_id FROM device WHERE fw_version < ?
                - canary:  加 ORDER BY random() LIMIT N%
                - beta:    JOIN user_device WHERE user_id IN (...)
                    ↓
              对每个 target device_id 并发 PUBLISH MQTT ota (QoS 1)
                    ↓
              INSERT ota_log (phase=downloading) for each
                    ↓
              设备上报 ota_progress → UPDATE ota_log.phase/percent
```

**并发控制**：100 个 device 并发下发，超过用 worker pool 限流。

**失败回滚**：某个设备失败不影响其他设备，自动重试 3 次（指数退避）。

### 4.5 OTA 进度聚合

```
[ESP32] → ota_progress {phase, percent}
    ↓
[MQTT Bridge] → 订阅 dev/+/telemetry → 解析 → UPDATE ota_log
    ↓
[REST API GET /firmwares/{id}/ota-progress]
    ↓
  聚合: COUNT(phase=ok), COUNT(phase=failed), AVG(percent WHERE phase=downloading)
    ↓
  返回:
  {
    chr(34)totalchr(34): 100,
    chr(34)okchr(34): 80,
    chr(34)failedchr(34): 5,
    chr(34)downloadingchr(34): 15,
    chr(34)failures_by_errorchr(34): {chr(34)sha256_mismatchchr(34): 3, chr(34)low_batterychr(34): 2}
  }
```

**Dashboard 轮询**：前端每 5s 拉一次。

## 5. REST API（详见 `docs/protocol/rest-api-v1.0.md`）

### 5.1 用户/小程序端 `/api/v1/*`

按域分组：

| 域 | 接口数 | 备注 |
|----|----|----|
| auth | 2 | wx-login, refresh |
| device | 6 | list/get/create/delete/patch/settings |
| room | 4 | list/create/update/delete |
| firmware/ota | 4 | list/create/publish/history |
| user | 2 | me, devices-summary |

**总计**：18 个接口

### 5.2 管理后台端 `/admin/*`

> **鉴权**：所有 `/admin/*` 接口需要 `Authorization: Bearer <jwt>`（管理员登录后获取）
> 或 `X-API-Key: sk-adm-{...}`。具体权限见 §5.3。

| Method | Path | 说明 | 权限 |
|----|----|----|----|
| POST | `/admin/auth/login` | 密码登录 → 返回临时 token | - |
| POST | `/admin/auth/mfa-verify` | TOTP 验证 → 返回正式 JWT | - |
| GET | `/admin/me` | 当前管理员信息 | admin JWT |
| GET | `/admin/dashboard/summary` | Dashboard 指标 | admin JWT |
| GET | `/admin/devices` | 设备列表 | `device:read` |
| GET | `/admin/devices/{id}` | 设备详情 | `device:read` |
| PATCH | `/admin/devices/{id}` | 编辑设备（名称/购买方等）| `device:write` |
| POST | `/admin/devices/{id}/regenerate-mgmt-token` | 重新生成 mgmt_token | `device:write` |
| POST | `/admin/devices/{id}/regenerate-acos-token` | 重新生成 acos_token | `device:write` |
| POST | `/admin/devices/{id}/command` | 远程指令（reboot/踢下）| `device:write` |
| DELETE | `/admin/devices/{id}` | 删除设备 | `device:delete` |
| POST | `/admin/devices/batch-command` | 批量指令 | `device:write` |
| GET | `/admin/api-keys` | API Key 列表 | admin JWT |
| POST | `/admin/api-keys` | 创建 API Key | admin JWT |
| DELETE | `/admin/api-keys/{id}` | 吊销 API Key | admin JWT |
| GET | `/admin/firmwares` | 固件列表 | `firmware:read` |
| POST | `/admin/firmwares` | 上传固件（multipart）| `firmware:write` |
| POST | `/admin/firmwares/{id}/publish` | 发布固件 | `firmware:write` |
| GET | `/admin/firmwares/{id}/ota-progress` | OTA 进度 | `firmware:read` |
| GET | `/admin/users` | 用户列表 | `user:read`（P1） |
| GET | `/admin/audit-log` | 审计日志 | admin JWT |

### 5.3 权限 scopes

| Scope | 含义 |
|----|----|
| `device:read` | 查看设备列表/详情 |
| `device:write` | 修改设备配置/重启/重新生成 token |
| `device:delete` | 删除设备记录 |
| `firmware:read` | 查看固件 |
| `firmware:write` | 上传/发布固件 |
| `user:read` | 查看用户 |
| `admin:all` | 超级管理员 |

## 6. MQTT Topic（详见 `docs/protocol/mqtt-v1.0.md`）

| Topic | 方向 | 用途 |
|----|----|----|
| `dev/{device_id}/cmd` | 云→设备 | config_update / command / ota |
| `dev/{device_id}/telemetry` | 设备→云 | auth / config_report / ota_progress |
| `dev/{device_id}/status` | broker LW | 异常掉线通知 |

## 7. 管理后台（Admin Dashboard）

> 管理后台是面向内部团队的 Web Dashboard，与微信小程序独立部署、独立鉴权，
> 是运营/运维/客户支持团队的工作入口。
>
> **技术选型**：React 18 + TypeScript + Vite + Ant Design 5  
> **部署**：独立子域名 `admin.example.com`，公网访问 + 密码 + TOTP MFA（强制开启）  
> **与本平台关系**：共享同一份 DB（`admin_user` / `api_key` / `admin_audit_log`），
> 调用同一份 `device` / `firmware` / `ota_log` 表（见 §3）。

> **数据模型**：`admin_user` / `api_key` / `admin_audit_log` 详见 §3.2。  
> **REST API**：`/admin/*` 详见 §5.2（含 scopes 权限矩阵）。  
 > **K8s 部署**：详见 §9.4。
 > **安全**：详见 §8.5。


### 7.1 背景与目标

本管理后台是云管平台的**运营 / 运维 / 客户支持**专用工具，给内部团队使用：

1. **运营**：查看全量设备状态、做 OTA 灰度、查运营指标
2. **运维**：设备上下线、远程重启、故障排查、API Key 管理
3. **客户支持**：根据 device_id 查设备所属公司/联系人、查看历史

**目标用户**：公司内部 IT 运维 / 客户支持 / 运营团队（≤ 50 人）

### 7.2 与其他端的关系

| 端 | 谁用 | 看什么 | 改什么 |
|----|----|----|----|
| **设备固件** | 不适用 | 不适用 | 自己的运行参数 |
| **微信小程序** | 终端用户 | 仅自己的设备配置 | 自己的设备配置 |
| **管理后台** | 内部团队 | **全部**设备、用户、固件、API Key | 全部 + 危险操作（踢下线 / 重新生成 token / OTA 发布） |

> 管理后台是**最高权限入口**，需要最强的安全控制（独立鉴权 + MFA + 审计）。

### 7.3 关键约束

- React 18 + TypeScript + Vite + Ant Design 5
- 部署在独立子域名 `admin.example.com`
- 公网访问 + 强密码 + TOTP MFA（强制开启）
- 管理员账号**不**走 wx.login，独立账号体系
- 所有写操作（踢下、重新生成 token、OTA 发布）记 `admin_audit_log`
- 不支持多租户（公司内部工具，本期不分租户）

### 7.4 路线图

| 期 | 范围 |
|----|----|
| **P0** | 登录（含 MFA）、Dashboard、设备管理、API Key 管理、固件管理 |
| **P1** | 用户/组织管理、审计日志、消息推送（钉钉/企业微信） |
| **P2** | SSO（接入公司 OAuth）、批量操作、设备使用统计 Dashboard |

### 7.5 信息架构

```
管理后台
├── 登录 / MFA 验证                无侧边栏的独立页
├── Dashboard (/)                  首页：聚合指标
├── 设备管理
│   ├── 设备列表 (/devices)
│   └── 设备详情 (/devices/:id)
├── API Key 管理 (/api-keys)
│   ├── API Key 列表
│   └── 创建 / 吊销
├── 固件管理 (/firmwares)
│   ├── 固件列表
│   ├── 上传固件
│   └── 发布 / 灰度策略
├── 用户/组织 (/users)             P1
└── 审计日志 (/audit-log)          P1
```

**顶部导航**（水平）：Korvo Logo ｜ Dashboard ｜ 设备 ｜ API Key ｜ 固件 ｜ 用户 ｜ 审计 ｜ 用户头像下拉

**侧边栏**：仅二级导航时显示

### 7.6 核心页面

### 7.6.1 登录 + MFA (`/login`)

```
┌──────────────────────────────────────────────────┐
│                                                  │
│              Korvo 管理后台                       │
│                                                  │
│       ┌────────────────────────────┐             │
│       │  邮箱 / 手机号             │             │
│       │  [___________________]    │             │
│       │  密码                       │             │
│       │  [___________________]    │             │
│       │                            │             │
│       │  [    登 录    ]           │             │
│       └────────────────────────────┘             │
│                                                  │
│   v1.0 · 账号问题请联系 IT                       │
└──────────────────────────────────────────────────┘
```

**首次登录流程**（强制 MFA 开启）：
```
[输入邮箱+密码] → [点击登录]
        ↓
[验证密码] → 成功 → [跳 MFA 页]
        ↓
[输入 6 位 TOTP]（Google Authenticator / 微软 Authenticator）
        ↓
[验证] → 成功 → [发管理员 JWT，写入 httpOnly cookie]
        ↓
[跳首页 Dashboard]
```

**首次登录后强制绑定 MFA**：
- 如果用户尚未绑定 TOTP → 跳 `/mfa-setup` 显示二维码
- 用 Authenticator 扫码 → 输入 6 位验证码 → 绑定完成
- **强制开启**，不能跳过

**安全措施**：
- 密码最少 12 位，必须含特殊字符
- 密码错误 5 次 → 锁定 15 分钟
- MFA 错误 5 次 → 锁定 1 小时
- Session 有效期 8 小时（管理员 JWT）
- 异地登录邮件钉钉 + 企业微信通知

### 7.6.2 Dashboard (`/`)

```
┌──────────────────────────────────────────────────────────────────┐
│ Korvo 管理后台    [Dashboard] 设备 API Key 固件 用户 审计   [admin]│
├──────────────────────────────────────────────────────────────────┤
│  Dashboard                            刷新: 5s 前    [立即刷新] │
│  ┌────────┬────────┬────────┬────────┐ │
│  │总设备  │ 在线    │ 24h活跃 │ 异常    │                          │
│  │ 1,234  │ 1,056  │  892   │  12    │                          │
│  └────────┴────────┴────────┴────────┘                          │
│  版本分布                购买方分布                                │
│  ┌────────────────┐     ┌────────────────┐                       │
│  │ v1.2.0   612  │     │ 企业  987        │                      │
│  │ v1.1.5   423  │     │ 个人  247        │                      │
│  │ v1.0.0   199  │     └────────────────┘                       │
│  └────────────────┘                                              │
│  在线/离线（24h）                  最近事件                          │
│  ┌────────────────────┐         ┌────────────────────────────┐│
│  │      ▁▂▃▄▅▆▇█       │         │ 10:23  OTA成功 korvo-005  ││
│  │                    │         │ 10:15  离线 korvo-023      ││
│  └────────────────────┘         │ 09:42  新设备注册 korvo-099 ││
│                                 └────────────────────────────┘│
└──────────────────────────────────────────────────────────────────┘
```

**指标聚合**（后端 `/admin/dashboard/summary` 接口）：
- `total_devices`: `SELECT COUNT(*) FROM device`
- `online_devices`: `SELECT COUNT(*) FROM device WHERE online=true`
- `active_24h`: `SELECT COUNT(*) FROM device WHERE last_seen_ts > now() - 86400`
- `error_devices`: 错误率 > 5% 的设备
- `version_distribution`: `SELECT fw_version, COUNT(*) GROUP BY fw_version`
- `buyer_distribution`: `SELECT buyer_type, COUNT(*) GROUP BY buyer_type`
- `online_trend_24h`: 每小时在线数（24 个点）

**刷新策略**：每 5s 自动刷新（页面聚焦时）

### 7.6.3 设备列表 (`/devices`)

```
┌──────────────────────────────────────────────────────────────────┐
│ 设备管理                                                            │
│ [搜索: device_id / 名称 / 序列号 / 购买方]  [筛选 ▼] [导出 CSV]   │
├──────────────────────────────────────────────────────────────────┤
│ ☐ │ 设备 ID │ 名称 │ 状态 │ 版本 │ 购买方 │ 联系人 │ 最后心跳 │  │
├──────────────────────────────────────────────────────────────────┤
│ ☐ │korvo-001│工位A│🟢在线│v1.2.0│ACME 公司│张工   │5min前│    │
│ ☐ │korvo-002│工位B│🟢在线│v1.2.0│ACME 公司│李工   │1min前│    │
│ ☐ │korvo-003│工位C│🔴离线│v1.0.0│某个人   │-      │3d前  │    │
│ ☐ │korvo-004│工位D│🟡异常│v1.1.5│企业B    │王工   │30min前│    │
│                                                                  │
│ [批量: 重启] [踢下线] [推送 OTA] [删除] [导出选中]                │
├──────────────────────────────────────────────────────────────────┤
│ 总 1,234 条  < 1 2 3 ... 247 >  每页 [50 ▼]   跳转到 [_] 页        │
└──────────────────────────────────────────────────────────────────┘
```

**筛选**：
- 状态：全部 / 在线 / 离线 / 异常（错误率 > 5%）
- 版本：下拉多选
- 购买方：企业 / 个人
- 时间：注册时间范围

**搜索**：支持模糊匹配 `device_id` / 名称 / 序列号 / 购买方 / 联系人

**批量操作**：
- 重启（→ `POST /admin/devices/batch-command {cmd: "reboot"}`）
- 踢下线（→ 强制断开 MQTT，EMQX ACL 拦截）
- 推送 OTA（→ 弹窗选版本）
- 删除（→ 二次确认 + 输入管理员密码）

**分页**：默认 50/页，支持 20/50/100/200

### 7.6.4 设备详情 (`/devices/:id`)

```
┌──────────────────────────────────────────────────────────────────┐
│ ← 设备详情    korvo-001 [🟢 在线]   [重启] [踢下线] [解绑] [更多▼] │
├──────────────────────────────────────────────────────────────────┤
│ 📋 基本信息                                       [编辑]           │
│   设备 ID       korvo-001                                          │
│   序列号        K001-A                                            │
│   设备名称      工位A001                                           │
│   型号          ESP32-S3-Korvo-2 V3.1                              │
│   所属组织      ACME 公司 > 北京办公室 > 工位区                    │
│   购买方类型    企业                                               │
│   购买方        ACME 公司                                          │
│   合同号        CT2025001                                          │
│   联系人        张工 13800001234                                   │
│   注册时间      2025-01-15 10:23                                   │
│                                                                  │
│ 🔐 鉴权信息                  [显示敏感字段]                          │
│   mgmt_token    sk-K001-7n3k9...      [复制] [重新生成]           │
│   acos_token    sk-baidu-...          [复制] [重新生成]           │
│   首次烧录      2025-01-15 10:25 by factory_burn.py               │
│                                                                  │
│ 📊 运行状态                                                       │
│   当前版本      v1.2.0                                            │
│   启动时间      2025-01-20 09:15                                  │
│   已运行时长    23h 15min                                         │
│   最后心跳      2025-01-21 08:30 (5min前)                         │
│   IP 地址       192.168.1.100                                     │
│   Wi-Fi RSSI    -45 dBm                                           │
│                                                                  │
│ ⚙️ 启用的功能                                                       │
│   [✓] ACOS AI 对话    [✓] 唤醒词检测    [✓] VAD 端点               │
│   [✓] Barge-in 打断   [ ] OTA 灰度测试组                            │
│                                                                  │
│ 🎛️ 当前用户配置                                                   │
│   音量       60        LED模式      night                          │
│   房间       卧室      AI 模型      korvo-v2                        │
│                                                                  │
│ 📜 历史记录                                                       │
│   [OTA 历史]  [操作日志]  [配置变更]  [错误日志]  [心跳曲线]        │
└──────────────────────────────────────────────────────────────────┘
```

**Tab 内容**：

#### OTA 历史
```
版本         阶段   开始          结束          耗时     操作
v1.0.0→v1.2.0 ok    2025-01-20 09:00  09:02  2min    [查看]
v1.0.0→v1.0.5 failed 2025-01-18 14:00 -      -       原因：电量不足 [查看]
```

#### 操作日志
```
时间              操作          操作人         结果
2025-01-21 08:30  配置变更      admin@corp    成功
2025-01-20 09:00  OTA 推送      ops@daily     成功
2025-01-15 10:25  烧录 mgmt_token factory_burn.py 成功
```

#### 配置变更
```
时间              字段    旧值   →   新值        操作人
2025-01-21 08:30  volume 50 → 60         user_u001
2025-01-20 09:00  led_mode normal → night ops@daily
```

#### 错误日志（最近 100 条）
```
时间              等级   消息
2025-01-21 08:15  ERROR  Wi-Fi connect timeout (3 retries)
2025-01-21 07:00  WARN   ACOS disconnected
```

#### 心跳曲线
- 过去 24h 的 RSSI / 在线状态 折线图
- 用 Recharts / Ant Design Charts

### 7.6.5 API Key 管理 (`/api-keys`)

```
┌──────────────────────────────────────────────────────────────────┐
│ API Key 管理                              [+ 创建 API Key]         │
├──────────────────────────────────────────────────────────────────┤
│  Key ID 前缀     名称         权限       状态 最后使用     创建  │
│  sk-adm-7n3k    ops-daily     3 项       活跃 2min前      张工   │
│  sk-adm-p3r8    dev-team      2 项       活跃 1d前        张工   │
│  sk-adm-q9k5    ci-deploy      2 项       活跃 5min前      王工   │
│  sk-adm-z1a2    old-key       3 项       已吊销 30d前    张工   │
│  sk-adm-m5n7    temp-key      1 项       已过期 1d前      李工   │
└──────────────────────────────────────────────────────────────────┘
```

**创建弹窗**：
```
┌──────────────────────────────────────────────────┐
│ 创建 API Key                                    [×]│
├──────────────────────────────────────────────────┤
│ 名称 *    [ops-daily]                            │
│                                                  │
│ 权限范围 *                                        │
│   [✓] device:read       查看设备列表/详情         │
│   [✓] device:write      修改设备配置/重启         │
│   [ ] device:delete     删除设备记录              │
│   [✓] firmware:read     查看固件                  │
│   [ ] firmware:write    上传/发布固件              │
│   [ ] user:read         查看用户                  │
│   [ ] admin:all         超级管理员                │
│                                                  │
│ 过期时间 *                                        │
│   ( ) 30 天后                                     │
│   ( ) 90 天后                                     │
│   (•) 180 天后                                    │
│   ( ) 1 年后                                      │
│   ( ) 永不过期（需超级管理员）                    │
│                                                  │
│     [取消]                          [创建]        │
└──────────────────────────────────────────────────┘

创建成功后（仅显示一次）：
┌──────────────────────────────────────────────────┐
│ ⚠️ 请立即保存完整 API Key                          │
│                                                  │
│ ┌────────────────────────────────────────────┐ │
│ │ sk-adm-7n3k9x2v8q5w1r2t3y4u5i6o7p8a9s0d │ [复制]│
│ └────────────────────────────────────────────┘ │
│                                                  │
│ 该 Key 仅显示一次！丢失需重新创建                 │
│                                                  │
│                       [我已保存，关闭窗口]        │
└──────────────────────────────────────────────────┘
```

**吊销确认**：
```
确定要吊销 sk-adm-7n3k (ops-daily) 吗？

该操作不可撤销，依赖此 Key 的服务将立即失效。

请输入您的管理员密码确认：[____________]

[取消]  [确认吊销]
```

### 7.6.6 固件管理 (`/firmwares`)

```
┌──────────────────────────────────────────────────────────────────┐
│ 固件管理                                  [+ 上传固件]             │
├──────────────────────────────────────────────────────────────────┤
│ 版本         大小     SHA256      状态    推送进度    发布时间     │
│ v1.2.0   1.2 MB a1b2c3..   已发布  95/100 成功      2025-01-20 │
│ v1.1.5   1.2 MB d4e5f6..   已发布  100% 完成      2025-01-10 │
│ v1.1.0   1.2 MB g7h8i9..   草稿   -            -             │
│ v1.0.5   1.1 MB j1k2l3..   已归档  -            2024-12-01   │
└──────────────────────────────────────────────────────────────────┘
```

**上传固件**：
```
┌──────────────────────────────────────────────────┐
│ 上传固件                                        [×]│
├──────────────────────────────────────────────────┤
│ 版本号 *   [v1.2.0]                              │
│                                                  │
│ 固件文件 *   [选择文件] korvo_agent.bin (1.2MB)  │
│                                                  │
│ 发版说明 *                                        │
│ ┌────────────────────────────────────────────┐ │
│ │ • 修复 Wi-Fi 重连 bug                        │ │
│ │ • 新增夜灯模式                              │ │
│ │ • 性能优化                                  │ │
│ └────────────────────────────────────────────┘ │
│                                                  │
│ 推送策略 *                                        │
│   ( ) 草稿（不推送）                              │
│   (•) 全量推送（所有设备立即推送）                │
│   ( ) 灰度 5%（随机 5% 设备）                    │
│   ( ) Beta 名单（指定用户组）                    │
│                                                  │
│ 升级窗口 *                                        │
│   [立即] / [延迟 1h] / [指定时间]                │
│                                                  │
│ 升级要求                                          │
│   [✓] 电量 > 30%                                │
│   [✓] 不在通话中                                │
│   [✓] 距离上次升级 > 24h                        │
│                                                  │
│     [取消]                          [上传并发布]  │
└──────────────────────────────────────────────────┘
```

**发布进度详情页 `/firmwares/:id`**：
- 实时进度条 + 各阶段设备数
- 失败原因饼图
- 设备列表（成功 / 失败 / 进行中），可手动重试

### 7.6.7 用户/组织 (`/users`) — P1

略，框架已搭。

### 7.6.8 审计日志 (`/audit-log`) — P1

所有写操作必须记录 `admin_audit_log`，结构：

| 字段 | 说明 |
|----|----|
| `log_id` | PK |
| `admin_user_id` | 哪个管理员 |
| `admin_api_key_id` | 用 API Key 还是 JWT |
| `action` | `device.reboot` / `api_key.create` / `firmware.publish` ... |
| `resource_type` | `device` / `api_key` / `firmware` |
| `resource_id` | 具体 id |
| `before` | JSONB（修改前） |
| `after` | JSONB（修改后） |
| `ip` | 操作者 IP |
| `ts` | UNIX 秒 |

UI 用 Ant Design Pro 的 ProTable，支持筛选 / 导出 CSV。
## 8. 安全

### 8.1 密钥管理

| 密钥 | 存储 | 用途 |
|----|----|----|
| `MGMT_TOKEN_SECRET` | Vault | 生成 mgmt_token |
| `DB_FIELD_KEY` (32 bytes) | Vault | AES-256-GCM 加密 DB 字段 |
| `JWT_SECRET` | Vault | 签发/校验 JWT |
| `WX_APPID/WX_SECRET` | Vault | 微信小程序对接 |
| `OSS_ACCESS_KEY/SECRET` | Vault | OSS 上传下载 |

### 8.2 网络

- API 服务暴露 HTTPS（443），后端服务互调用走 K8s 内网
- EMQX 暴露 mqtts 8883，禁用 1883 明文端口
- 数据库不暴露公网，仅 RDS 内网访问

### 8.3 限流

- 单 IP：100 req/min（防刷）
- 单 user：60 req/min
- 单 device MQTT：500 msg/min

### 8.4 审计

- 所有 `config_change_log` 永久可查（30 天后清理原始日志）
- 所有 OTA 上传记录 firmware 创建者 + IP
- 所有管理员写操作记录 `admin_audit_log`（保留 1 年，见 §3.2）

### 8.5 管理后台特有威胁与缓解

| 威胁 | 缓解措施 |
|----|----|
| 暴力破解密码 | bcrypt + 5 次失败锁定 15 分钟 |
| 暴力破解 MFA | 5 次失败锁定 1 小时 + 邮件告警 |
| Session 劫持 | JWT 写入 httpOnly + Secure cookie |
| CSRF | SameSite=Strict + CSRF token |
| XSS | React 自动转义 + CSP Header |
| 越权访问 | RBAC（每个 API 检查 scopes，见 §5.3） |
| 内部恶意操作 | 审计日志 + 二次密码确认（吊销/重生成） |
| API Key 泄漏 | 完整 key 仅显示一次 + 90 天过期强制续期 |
| 管理后台被入侵后批量破坏 | 关键操作需重新输入密码确认 |

## 9. 部署 / 监控 / 告警

### 9.1 部署清单

| 组件 | 资源 | 副本数 |
|----|----|----|
| REST API | 1 CPU / 1GB | 2 |
| MQTT Bridge | 1 CPU / 512MB | 2 |
| EMQX | 2 CPU / 4GB | 3 (StatefulSet) |
| Postgres | 4 CPU / 16GB | 1 主 + 1 备 (RDS) |
| Redis | 1 CPU / 1GB | 3 (Cluster) |
| admin-web (Nginx 静态) | 100m / 128MB | 2 |
| admin-api (Gin handler) | 1 CPU / 1GB | 2 |

### 9.2 Prometheus 指标

- `korvo_api_request_duration_seconds{path, status}`
- `korvo_mqtt_connected_devices`
- `korvo_mqtt_messages_total{topic, type}`
- `korvo_ota_progress{phase}`
- `korvo_db_pool_active`

### 9.3 关键告警

| 指标 | 阈值 | 通知 |
|----|----|----|
| MQTT 连接数突降 | 5min 内下降 >20% | 钉钉群 |
| OTA 失败率 | 5min 内 >5% | 钉钉群 + 短信 |
| API 5xx 错误率 | 1min 内 >1% | 钉钉群 |
| Postgres 连接数 | >80% | 钉钉群 |
| Redis 内存 | >70% | 钉钉群 |
| 管理后台登录失败率 | 5min 突增 | 钉钉群 + 邮件 |
| 管理后台审计日志写入失败 | >0 | 钉钉群 + 短信 |

### 9.4 管理后台域名 + Ingress

| 子域名 | 用途 |
|----|----|
| `api.example.com` | REST API |
| `admin.example.com` | 管理后台 SPA |

**Ingress + TLS**（`admin.example.com`，cert-manager 自动续 Let's Encrypt）：

```yaml
apiVersion: networking.k8s.io/v1
kind: Ingress
metadata:
  name: admin-web
spec:
  rules:
  - host: admin.example.com
    http:
      paths:
      - path: /
        pathType: Prefix
        backend:
          service:
            name: admin-web
            port:
              number: 80
      - path: /api
        pathType: Prefix
        backend:
          service:
            name: admin-api
            port:
              number: 8080
  tls:
  - hosts:
    - admin.example.com
    secretName: admin-tls
```

**反向代理**：前端 SPA + 后端 API 同源（避免 CORS），通过 Ingress 路由区分：
- `/` → 前端静态
- `/api/*` → admin-api 服务

## 10. Open Issues

### 10.1 云平台 OI

| # | 问题 | 影响 | 状态 |
|---|----|----|----|
| OI-CP-01 | 多租户支持（不同代理商隔离） | P2 才需要 | 待定 |
| OI-CP-02 | EMQX vs Mosquitto 选型最终定 | 部署 | 已选 EMQX |
| OI-CP-03 | mTLS 是否启用 | 安全 | 待 OI-05 拍板 |
| OI-CP-04 | OSS 走不走 CDN 走自建 | 成本 vs 性能 | 待定 |
| OI-CP-05 | OTA 进度聚合实时 vs 轮询 | 前端体验 | 推荐 5s 轮询 |
| OI-CP-06 | 设备禁用/踢下线的实现 | 安全 | 待实现 |
| OI-CP-07 | 是否支持远程日志上报 | 调试 | OI-18 之后 |

### 10.2 管理后台 OI


| # | 问题 | 影响 | 状态 |
|---|----|----|----|
| OI-AD-01 | 是否接入公司 SSO（OAuth/SAML）| 减少账号体系 | P2 |
| OI-AD-02 | MFA 是否强制（不能跳过） | 安全 | 已选强制 |
| OI-AD-03 | 管理后台是否需要审批流（敏感操作要 2 人）| 安全 | P2 |
| OI-AD-04 | 是否支持设备标签（tag）便于筛选 | 运营 | P1 |
| OI-AD-05 | 是否做"客户自助 Portal"（让 ACME 公司自己看他们的） | B2B | P2 |


*Created: 2026-09-24 — 与设备侧 PRD §11I 同步。本版本整合了原 PRD-admin-dashboard.md 的内容。*
