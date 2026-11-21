# 本地云端接收服务

已经实现：设备批量接收、数据库提交后业务确认、重发/补传去重、单位隔离查询、射击去重和按事件日期统计、本地身份管理工具、自动化接口验证。

2026-11-17 已部署到现有阿里云 ECS。首台 ESP32-S3 使用公网 IP 的 HTTPS 上传入口，访问来源限制为测试 Wi-Fi 的公网出口。真实设备数据写入运行库；自动化测试仍使用独立的临时数据库。参见[云端部署记录](../docs/云端接收服务部署记录.md)与[设备补传联调](../deploy/设备补传联调.md)。

## 1. 安装与启动（Windows PowerShell）

在 `D:\Gun_Detection_Project` 执行，使用 Python 3.10 或更新版本：

```powershell
python -m venv cloud_backend/.venv
cloud_backend/.venv/Scripts/python.exe -m pip install -r cloud_backend/requirements-dev.txt
./cloud_backend/start.ps1
```

运行依赖为 `requirements.txt`；开发/验证依赖为 `requirements-dev.txt`。环境已存在时不必每次安装。

- 接口说明及交互调试入口：<http://127.0.0.1:8000/docs>
- 存活/存储检查：<http://127.0.0.1:8000/healthz>
- OpenAPI 定义：<http://127.0.0.1:8000/openapi.json>
- 停止：在服务窗口按 `Ctrl+C`。
- 改端口：`./cloud_backend/start.ps1 -Port 8001`。

服务默认只监听本机，不需要管理员权限。默认数据库为 `cloud_backend/runtime/receiver.db`，启动时初始化，重新启动保留数据。它与 `data_monitor/gun_telemetry.db` 完全独立，不做自动历史导入。

## 2. 登记真实单位与设备

另开 PowerShell，在项目根目录执行：

```powershell
$pythonPath = './cloud_backend/.venv/Scripts/python.exe'
& $pythonPath -m cloud_backend.admin create-tenant --id unit-a --name '实际单位名称'
& $pythonPath -m cloud_backend.admin register-device --tenant unit-a --id gun_dection --name '检测设备001'
```

`gun_dection` 是当前固件中的设备编号示例。正式接入时应逐台配置唯一编号，上传的 deviceId 必须和登记一致。这里仅登记身份，不会启动或模拟任何设备。

第一条命令显示 `queryToken`，只允许查询该单位的数据；第二条显示 `deviceToken`，只允许对应设备上传。随机令牌仅创建时显示一次，数据库保存 SHA-256 摘要。不要把输出放入源码、Git 或公开日志。没有默认通用密码，没有公开的管理员登记接口；重复登记会报错，不覆盖已有记录。

登记另一个单位时改用不同的单位 ID，不同设备也必须使用不同设备 ID。首版每个查询令牌只属于一个单位，尚未实现用户登录、成员邀请或跨单位管理员视图。

禁用凭据但保留历史：

```powershell
& $pythonPath -m cloud_backend.admin disable-device --id gun_dection
& $pythonPath -m cloud_backend.admin disable-readers --tenant unit-a
```

首版不提供单位/设备转移或令牌重置功能；不要通过编辑数据库改变已有数据的归属。

## 3. 上传协议

`POST /device/v1/batches`，请求头：

```text
Authorization: Bearer <逐台设备的 deviceToken>
Content-Type: application/json
```

请求体示意（仅协议示例，不会自动发送）：

```json
{
  "schemaVersion": 1,
  "deviceId": "gun_dection",
  "storageEpoch": "first-storage-epoch",
  "batchId": "batch-000001",
  "format": "compact-v2-string-u64",
  "records": [
    ["1001", "2026-11-16 09:00:00", 1, 2, 3, 4, 5, 0.1,
     31.0, 121.0, "0123456789abcdef", "120000", 0, 2]
  ]
}
```

字段顺序兼容 `data_monitor/monitor_lib/transfer_control.py`：

```text
sequence, timestamp, accel_x, accel_y, accel_z, mic_raw, mic_p2p,
mic_rms, latitude, longitude, bootId, uptimeMs, shotId, recordKind
```

- 支持 `compact-v1`（13 列，没有 recordKind）、`compact-v2`（14 列）和 `compact-v2-string-u64`（14 列）。旧格式可传整数；64 位数值建议使用十进制字符串，响应始终以字符串返回 sequence。
- `schemaVersion` 必须是整数 1；每批 1～64 条，请求体实际接收量不超过 64 KiB；当前设备端后续接入建议先使用每批 20 条。
- bootId 为原记录的 16 位十六进制字符串，不能用上传时的启动标识覆盖历史记录的 bootId。
- storageEpoch 是设备存储世代，普通重启不变；真正重置存储才更换。此字段当前由上传客户端提供，服务端不自动生成或变更。
- batchId 标识一个不可变批次；超时后可原批重发，也可使用新 batchId 重组上传。同一 batchId 对应不同内容返回 409。
- 记录唯一键为 `(deviceId, storageEpoch, bootId, sequence)`；相同身份和规范化内容重传返回成功，相同身份不同内容返回 409。不同设备的相同序号不会互相冲突。
- 单批存在重复身份、字段类型错误、越界值、非法时间或半个定位坐标时，整批拒绝，不提交部分记录。
- `recordKind`：0 旧版/组合快照，1 传感器，2 GPS。shotId 大于 0 时按射击事件处理。
- 同一 `(deviceId, storageEpoch, bootId, shotId)` 只计一次射击；同身份的时间/位置不同则返回冲突，不覆盖历史。
- 日历字符串按北京时间解释，同时保存 UTC 和原字符串；`T+HH:MM:SS` 保留相对时间，不伪造日历日期。
- latitude/longitude 必须同时为有效范围内数值或同时为 null；0,0 标为无效定位，原始值保留，不当作真实位置。
- 不接收 tenantId。服务端根据已认证设备的登记关系确定单位。

成功响应示意：

```json
{
  "committed": true,
  "deviceId": "gun_dection",
  "storageEpoch": "first-storage-epoch",
  "batchId": "batch-000001",
  "acceptedCount": 1,
  "insertedCount": 1,
  "duplicateCount": 0,
  "records": [{"bootId": "0123456789abcdef", "sequence": "1001"}]
}
```

设备必须同时检查 committed、deviceId、epoch、batchId 和完整记录身份列表后才确认本地缓存，不能只凭 HTTP 200 或最大序号回收。重发已入库记录时 insertedCount=0、duplicateCount=记录数，仍返回 committed=true。序号跳跃合法。

写入记录、射击派生表、批次凭据和 outbox 在同一事务中；提交失败返回 503、无 committed 确认，设备保留原数据后重试。当前主记录表的唯一约束兼作逐记录凭据，因此本阶段不清理该表；后续实施归档时必须保留独立去重凭据再清理明细。

状态码：401 缺少/无效/停用凭据；403 凭据与 deviceId 不符；409 身份/批次内容冲突；413 超出大小；422 格式错误；503 存储失败。503 建议等待响应中的 Retry-After 秒数后重试；内容冲突不要无限快速重传或擅自丢记录。

## 4. 单位查询接口

在 `/docs` 右上角 Authorize 中输入该单位的 `queryToken`（不加 Bearer 前缀）。设备上传时改用 deviceToken，两类凭据不能互用。

| 接口 | 功能 |
|---|---|
| `GET /api/v1/devices` | 本单位设备列表，不返回凭据 |
| `GET /api/v1/records` | 持久化明细，支持 deviceId、kind、eventDate、limit、offset |
| `GET /api/v1/records/{record_id}` | 单条明细；跨单位和不存在都返回 404 |
| `GET /api/v1/shots` | 唯一射击事件，支持 deviceId、eventDate、分页 |
| `GET /api/v1/statistics` | 本单位或本单位某设备的记录数、唯一射击数、按事件日期统计 |

列表默认 100 条，上限 200；kind 为 sensor/gps/shot；eventDate 为北京时间事件日期，例如 `2026-11-15`。未校时事件的统计 date 为 null，旧数据今天上传仍归其原发生日期。列表默认按接收时间和记录 ID 稳定排列，不按字符串序号排序；同一接收时刻的顺序不代表采集顺序，应结合 bootId/uptimeMs。

所有查询在服务端限定单位。用甲单位令牌指定乙单位设备返回 404，猜测乙单位记录 ID 同样返回 404；客户端不能用参数切换单位。当前为应用层隔离，未部署 PostgreSQL 行级策略、多人用户认证、浏览器会话、下载授权或导出服务。

## 5. 运行验证

```powershell
cloud_backend/.venv/Scripts/python.exe -m pytest cloud_backend/tests -q
```

测试使用 pytest 临时目录，测试结束不污染运行库。覆盖：正常上传、响应丢失后的重发、跨批次补传、服务重启持久性、并发重传、整批原子性、数据库写入和提交失败、逐台认证、停用、跨单位查询/详情/统计、迟到日期、未校时、uint64 和旧协议格式、请求体限制及非法输入。

这些是接口测试，不是四个常驻模拟设备，也没有进行 5 台实机或吞吐容量验收。

## 6. 阿里云接入前的边界

- 本地已采用可切换数据库的 SQLAlchemy 结构。默认 SQLite 使用 WAL/FULL 和有界等待，事务串行化以保证本地正确性，适合本地联调，不据此宣称生产容量。
- 可通过 `GUN_DATABASE_URL` 指定独立数据库，例如 `postgresql+psycopg://...`，届时另安装 psycopg 驱动；目前 PostgreSQL/RDS 未实机验证。云端部署前需验证权限、锁并发、备份恢复，补齐版本化迁移与行级隔离策略。
- 现在只有接收、查询和聚合，没有 OSS 归档 worker、设备状态心跳、远程命令或后台用户页面。outbox 只持久记录待归档任务，不声称已归档或自动清理。
- 现在没有修改 ESP32 固件。真实设备仍使用原局域网上传，直到实现 HTTPS 客户端、配置和云端确认验证后才能直接接入。
- 本地入口使用 HTTP/回环地址；阿里云正式入口必须配置域名、TLS、反向代理、请求限流、日志和运维权限，再做正式接入验证。

实现依据：[FastAPI 认证依赖](https://fastapi.tiangolo.com/tutorial/security/first-steps/)、[SQLAlchemy 事务](https://docs.sqlalchemy.org/en/20/orm/session_transaction.html)、[SQLite 事务及连接配置](https://docs.sqlalchemy.org/en/20/dialects/sqlite.html)。
