# 新设备完整部署指南：云端配置、固件烧录与验收

适用项目：Gun_Detection_Project。根据 2026-11-22 本地代码整理；本文不表示已执行云端部署或实机烧录。

当前实现为 ESP32-S3 → HTTPS → Ubuntu 接收服务 → SQLite → 只读查询网页。以下命令中的示例设备、单位、版本、串口和地址须替换为实际值；文档不保存真实密码、Token 或私钥。

## 1. 先判断要走哪条流程

| 场景 | 需要执行的步骤 |
|---|---|
| 现有服务器已经支持 `compact-v4-time`，只增加一台设备 | 核对服务 → 第 5 节注册设备 → 第 6～8 节配置、烧录、验收 |
| 现有服务器仍是旧协议 | 先执行第 3～4 节更新接收服务和网页，再接入设备 |
| 全新 Ubuntu 服务器 | 第 2～8 节全部执行；先准备服务器地址、TLS 证书和 HTTPS 入口 |
| 只删除网页中的“角度状态 / 历史 ADC”列 | 只更新 `viewer.py` 并重启 `gun-viewer`，不会改变设备上传协议 |

一台服务器可登记多台设备；同一单位新增设备不需要重新创建单位，也不需要重装服务。

## 2. 准备清单

### 2.1 云端与本地

- 可登录的 Ubuntu 服务器，具有 sudo 权限；已安装 Python 3、venv、curl、Nginx，使用 systemd。
- 本地项目路径：`D:\Gun_Detection_Project`。
- 本地 Python、PlatformIO；能够识别 ESP32-S3 的 USB 串口。
- 设备为与当前配置匹配的 ESP32-S3，Flash 为 16 MB；其他硬件需先核对板型、引脚和分区，不能直接套用。
- 设备可连接的 2.4 GHz Wi-Fi；可访问 HTTPS 上传入口和校时服务。
- 每台设备分配唯一 `deviceId`，确定其所属 `tenantId`。

### 2.2 当前项目路径与服务

| 项目 | 默认路径 / 名称 |
|---|---|
| 接收服务 | `gun-receiver.service`，回环端口 `18000` |
| 网页服务 | `gun-viewer.service`，回环端口 `18001` |
| 接收程序 | `/opt/gun-receiver/current`，指向某个 releases 版本目录 |
| 网页程序 | `/opt/gun-viewer/viewer.py` |
| 地图模板 | `/opt/gun-viewer/viewer_map.html` |
| 接收服务环境 | `/etc/gun-receiver.env` |
| 默认 SQLite | `/var/lib/gun-receiver/receiver.db` |
| 地图配置 | `/opt/gun-viewer/amap_config.json` |
| 当前源码上传入口 | `https://39.106.146.122/device/v1/batches` |
| 当前项目记录的查询入口 | `https://39.106.146.122/viewer/` |

上述公网地址来自项目配置，部署当天仍需实际验证。查看当前云端服务配置：

```bash
sudo systemctl cat gun-receiver
sudo systemctl cat gun-viewer
sudo systemctl status gun-receiver gun-viewer nginx --no-pager
```

新服务器若缺少基础工具，可先执行：

```bash
sudo apt-get update
sudo apt-get install -y python3 python3-venv curl nginx
```

## 3. 安装或升级云端接收服务

必须先让接收端支持 `compact-v4-time`，再烧录新版云端固件，否则设备上传会被旧服务拒绝。

### 3.1 本地验证并打包（Windows PowerShell）

```powershell
Set-Location D:\Gun_Detection_Project
# 首次配置本地 Python 环境才需要以下两行；已有环境可以跳过。
python -m venv cloud_backend/.venv
cloud_backend/.venv/Scripts/python.exe -m pip install -r cloud_backend/requirements-dev.txt

cloud_backend/.venv/Scripts/python.exe -m pytest cloud_backend/tests -q
# 测试通过后再执行打包；每次发布使用新的版本名。
cloud_backend/.venv/Scripts/python.exe deploy/build_bundle.py --release receiver-20261122-device-01
```

生成文件：

```text
output/deployment/receiver-20261122-device-01.tar.gz
output/deployment/receiver-20261122-device-01.tar.gz.sha256
```

打包脚本只收集后端 Python、运行依赖和 Ubuntu 部署文件，不包含数据库、设备私有配置、固件及地图 HTML。修改源码后必须重新打包；以前上传的压缩包不会自动更新。

### 3.2 上传到服务器

使用阿里云终端的上传功能，将以下四个文件上传到 `/tmp`：

1. 新生成的 `.tar.gz`。
2. 同名 `.tar.gz.sha256`。
3. `cloud_backend/viewer.py`。
4. `cloud_backend/viewer_map.html`。

必须确认每个文件上传成功。如果出现 `Session has expired`，重新连接终端后重新上传失败文件；不要直接使用可能残留的同名旧文件。也可以使用自己已配置好认证的 SCP/SFTP。

### 3.3 升级前备份（已有服务器）

以下命令适用于默认 SQLite 路径，以 SQLite 在线备份方式保持一致性，不直接复制运行中的数据库主文件：

```bash
sudo bash <<'BASH'
set -euo pipefail
backup_dir="/var/backups/gun-receiver/$(date +%Y%m%d-%H%M%S)"
install -d -m 0700 "$backup_dir"
if [ -f /var/lib/gun-receiver/receiver.db ]; then
  python3 - "$backup_dir/receiver.db" <<'PY'
import sqlite3, sys
with sqlite3.connect('file:/var/lib/gun-receiver/receiver.db?mode=ro', uri=True) as src:
    with sqlite3.connect(sys.argv[1]) as dst:
        src.backup(dst)
PY
fi
if [ -f /etc/gun-receiver.env ]; then
  cp -a /etc/gun-receiver.env "$backup_dir/"
fi
if [ -d /opt/gun-viewer ]; then
  cp -a /opt/gun-viewer "$backup_dir/viewer"
fi
if [ -L /opt/gun-receiver/current ]; then
  readlink -f /opt/gun-receiver/current > "$backup_dir/previous-release.txt"
fi
echo "Backup: $backup_dir"
BASH
```

若实际数据库地址不同，先按实际配置修改备份路径。备份包含凭证哈希和配置，应保持私有。

### 3.4 校验、解包、安装（服务器终端）

```bash
sudo bash <<'BASH'
set -euo pipefail
release_name=receiver-20261122-device-01
cd /tmp
# 项目生成的 sha256 文件只有摘要，没有文件名，不能直接 sha256sum -c。
expected=$(tr -d '\r\n' < "$release_name.tar.gz.sha256")
actual=$(sha256sum "$release_name.tar.gz" | cut -d ' ' -f 1)
test "$actual" = "$expected"
release_dir="/opt/gun-receiver/releases/$release_name"
# 不覆盖已有发布目录；如已存在，换一个新版本名重新打包。
test ! -e "$release_dir"
install -d -m 0755 "$release_dir"
tar -xzf "$release_name.tar.gz" -C "$release_dir"
bash "$release_dir/deploy/ubuntu/install.sh"
BASH
```

安装脚本会创建运行用户、虚拟环境、默认环境文件（仅当文件不存在）、切换 `current` 并启动接收服务。它保留已有数据库配置，不负责配置公网 HTTPS，也不安装网页服务。

检查：

```bash
curl --fail http://127.0.0.1:18000/healthz
sudo systemctl status gun-receiver --no-pager
```

## 4. 安装或更新网页与公网入口

### 4.1 更新网页

确认服务使用默认的 `/opt/gun-viewer` 路径后执行：

```bash
sudo bash <<'BASH'
set -euo pipefail
python3 -c "import ast; ast.parse(open('/tmp/viewer.py', encoding='utf-8').read()); print('viewer syntax OK')"
test -s /tmp/viewer_map.html
install -d -m 0755 /opt/gun-viewer
if [ -f /opt/gun-viewer/viewer.py ]; then
  cp -a /opt/gun-viewer/viewer.py "/opt/gun-viewer/viewer.py.bak.$(date +%Y%m%d-%H%M%S)"
fi
install -m 0644 /tmp/viewer.py /opt/gun-viewer/viewer.py
install -m 0644 /tmp/viewer_map.html /opt/gun-viewer/viewer_map.html
# 首次安装才复制 service；已有服务保留现场配置。
if [ ! -f /etc/systemd/system/gun-viewer.service ]; then
  install -m 0644 /opt/gun-receiver/current/deploy/ubuntu/gun-viewer.service /etc/systemd/system/gun-viewer.service
fi
systemctl daemon-reload
systemctl enable gun-viewer
systemctl restart gun-viewer
curl --fail http://127.0.0.1:18001/viewer/ > /dev/null
systemctl status gun-viewer --no-pager
BASH
```

网页默认读取上述 SQLite 文件。如果接收服务使用了不同 SQLite 路径，应通过 `sudo systemctl edit gun-viewer` 增加如下配置，再执行 `daemon-reload` 和重启：

```ini
[Service]
Environment=VIEWER_DB=/实际路径/receiver.db
```

当前网页直接使用 SQLite，不能仅把这个变量改成 PostgreSQL URL。网页服务保持单 worker；会话保存在内存，重启后需重新登录。

### 4.2 HTTPS 与 Nginx

现有服务器如果上传、网页登录都正常，保留其配置；增加设备无需修改 Nginx 或安全组。

全新服务器需要先准备匹配实际域名/IP 的服务器证书、完整证书链与私钥，并确认设备信任对应根 CA。当前项目使用私有测试 CA；新终端访问时需要正确安装该 CA，不能靠关闭证书验证完成验收。更换服务器地址时还必须同步修改固件上传 URL。

在专用新入口的 HTTPS `server` 块中配置以下路由（证书路径和 `server_name` 按实际填写）：

```nginx
server {
    listen 443 ssl;
    server_name YOUR_HOST;
    ssl_certificate /etc/gun-receiver/tls/chain.pem;
    ssl_certificate_key /etc/gun-receiver/tls/server.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    client_max_body_size 64k;
    client_body_timeout 15s;

    location = /device/v1/batches {
        limit_except POST { deny all; }
        proxy_pass http://127.0.0.1:18000;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-Proto https;
        proxy_set_header X-Forwarded-For $remote_addr;
        proxy_read_timeout 30s;
    }
    location = /viewer { return 302 /viewer/; }
    location /viewer/ {
        proxy_pass http://127.0.0.1:18001;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-Proto https;
        proxy_set_header X-Forwarded-For $remote_addr;
    }
    location / { return 404; }
}
```

不要用仓库里的 `device-upload.nginx.conf.in` 原样覆盖现有配置：它仍含 `TEST_WIFI_IPV4` 来源限制，且没有网页路由。上述配置为接入基线，不代表已具备登录限流、多角色管理或完整生产安全功能。

配置后执行：

```bash
sudo nginx -t && sudo systemctl reload nginx
```

云安全组及主机防火墙允许所需来源访问 TCP 443。需要跨网络上传/查询时，443 的来源规则应匹配该需求；SSH 仅开放管理来源。不公开 `18000`、`18001` 或数据库端口。

### 4.3 地图（需要轨迹底图时）

准备适用于实际网页地址的高德 Web 端 Key 与安全配置。按当前代码，配置文件格式为：

```json
{
  "key": "替换为地图Key",
  "securityJsCode": "替换为对应安全密钥"
}
```

写入 `/opt/gun-viewer/amap_config.json`，确保 `gun-receiver` 用户可读，保留现有正确配置。地图服务授权范围按地图平台配置。地图未配置不代表设备上传失败；同一设备同一次上电至少有 3 个有效定位点才列出轨迹。

## 5. 云端登记单位和新设备

### 5.1 首次创建单位

以下示例使用单位 `unit-a`。已有该单位时跳过，沿用原查询凭证。

```bash
sudo bash <<'BASH'
set -euo pipefail
set -a
source /etc/gun-receiver.env
set +a
: "${GUN_DATABASE_URL:?Missing database URL}"
cd /opt/gun-receiver/current
runuser -u gun-receiver -- env GUN_DATABASE_URL="$GUN_DATABASE_URL" \
  /opt/gun-receiver/current/.venv/bin/python -m cloud_backend.admin \
  create-tenant --id unit-a --name '单位A'
BASH
```

保存输出的 `queryToken`：网页登录的“用户账号”为 `unit-a`，“查询密码”为这个 Token。它不是阿里云账号密码，也不是设备上传 Token。当前实现是单位级查询凭证，不是每个员工一个独立账号。

### 5.2 每台新设备都要单独登记

以下示例增加 `device-002`：

```bash
sudo bash <<'BASH'
set -euo pipefail
set -a
source /etc/gun-receiver.env
set +a
: "${GUN_DATABASE_URL:?Missing database URL}"
cd /opt/gun-receiver/current
runuser -u gun-receiver -- env GUN_DATABASE_URL="$GUN_DATABASE_URL" \
  /opt/gun-receiver/current/.venv/bin/python -m cloud_backend.admin \
  register-device --tenant unit-a --id device-002 --name '检测设备002'
BASH
```

保存输出的 `deviceToken`，仅写入这台设备。下一台使用 `device-003` 等新的唯一编号并重新登记，不能共用上一台设备的 Token 或已编译固件。

始终加载 `/etc/gun-receiver.env` 并以服务用户执行，避免误在当前目录创建另一份数据库。Token 仅创建时显示明文；数据库保留摘要。重复登记不会覆盖原设备，当前 CLI 也没有找回原 Token 或直接轮换 Token 的命令。

## 6. 配置设备固件

### 6.1 私有身份配置

编辑本地 `gun_detection/include/cloud_device_private.h`。保留代码所用的四个变量名，示例：

```cpp
#pragma once
static const char archiveDevice[] = "device-002";
static const char archiveEpoch[] = "替换为本设备存储代号";
static const char archiveToken[] = "替换为本设备deviceToken";
static const char archiveRoot[] = R"PEM(-----BEGIN CERTIFICATE-----
替换为完整根CA证书内容
-----END CERTIFICATE-----
)PEM";
```

可在 PowerShell 生成新的存储代号：

```powershell
[guid]::NewGuid().ToString('N')
```

- `archiveDevice` 必须与云端注册的设备 ID 完全一致。
- `archiveEpoch` 表示这一代记录存储：新设备首次初始化生成一次；正常重启和保留缓存的升级必须沿用；明确重新初始化记录存储时才更换。
- `archiveToken` 是该设备的上传凭证；`archiveRoot` 是验证服务端的根证书，不能填私钥。
- 该头文件已被 Git 忽略。固件二进制同样含凭证，不公开共享，也不要把带凭证的整套 Flash 复制给其他设备。

### 6.2 Wi-Fi、时间段与上传地址

编辑 `gun_detection/include/system_config.h`：

| 配置 | 接入时检查内容 |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | 当前设备使用的 Wi-Fi 名称和密码 |
| `DEVICE_ID` | 建议与 `archiveDevice` 一致，避免本地状态与云端编号混淆；HTTPS 身份实际取 `archiveDevice` |
| `NORMAL_WIFI_ALWAYS_ON` | 首次联调设 `true`，普通工作模式自动联网 |
| `NTP_SERVER` | 设备网络可访问的校时服务器 |
| `NIGHT_STANDBY_ENABLED` | 是否启用每日定时休眠 |
| `NIGHT_STANDBY_START_HOUR/MIN` | 北京时间开始休眠的时刻 |
| `NIGHT_STANDBY_END_HOUR/MIN` | 北京时间恢复工作的时刻 |
| `NORMAL_SENSOR_RECORD_INTERVAL_MS` | 普通模式传感器保存周期，当前 10000 ms |
| `LOW_POWER_SENSOR_RECORD_INTERVAL_MS` | 低功耗模式传感器保存周期，当前 10000 ms |

本次检查时源码为 09:59 休眠、10:00 恢复，这是联调设置，交付前应改成所需工作时间。静止低功耗还可能关闭无线电；`NORMAL_WIFI_ALWAYS_ON=true` 不代表任何模式都永久联网。

上传 URL 当前写在 `gun_detection/src/archive_upload.cpp` 的 `config.url` 中，不在上述私有头文件。使用另一台服务器时修改 URL，同时核对证书与访问主机匹配。只更换 Wi-Fi、不更换服务器时无需修改 URL。

Wi-Fi 配置所在文件是项目源码，提交前检查差异，避免把实际密码提交到公共仓库。

### 6.3 当前到底上传什么

新版 `esp32-s3-cloud` 的新传感器记录上传 Roll、Pitch、角度有效标记、麦克风数据以及有效的 GPS 信息；不再上传原始 X/Y/Z ADC。事件和 GPS 记录按各自语义处理。

三轴信号仍在设备内部采样，用于角度计算和动作检测。刚上电约 2 秒应保持静止、水平；未校准或无效角度上传空值，不把无效数据当作 0°。当前角度是基于加速度的估计，不提供航向角。

旧 Flash 中原本保存的 ADC 记录仍按旧语义补传，以保证已上传记录重试时不发生内容冲突。删除网页列不改变这一点；本指南不清空旧缓存或删除云端历史记录。

### 6.4 未校时记录自动推算

同次上电后续获得有效 UTC 后，设备保存固定时间基准，上传此前记录时将 `T+…` 转成推算的北京时间，并附带原始时间。网页按生成时间排序并标记“推算”。基准持久化保存，可供对应 boot 重启后补传；没有该 boot 时间基准时不猜测日期。

此功能需要同时更新接收服务、网页和固件，不能仅替换网页。详见 [时间推算与上传说明](inferred_capture_time.md)。

## 7. 编译与烧录

### 7.1 编译（本地 PowerShell）

```powershell
Set-Location D:\Gun_Detection_Project\gun_detection
pio device list
pio run -e esp32-s3-cloud
```

若 `pio` 未在 PATH 中，使用 VS Code 的 PlatformIO 终端。必须选择 `esp32-s3-cloud`，不要选择普通版、容量测试版或待机测试版。

固件生成位置：

```text
gun_detection/.pio/build/esp32-s3-cloud/firmware.bin
```

### 7.2 全新设备首次烧录

确认是真正的新板、16 MB Flash、引脚与项目相符，并且没有需要保留的历史数据。关闭占用串口的监视器，将 `COM5` 换成实际端口：

```powershell
pio run -e esp32-s3-cloud -t upload --upload-port COM5
pio device monitor --port COM5 --baud 115200
```

首次上传会按 PlatformIO 配置写入启动组件、分区表和应用。若不能自动进入下载模式：按住 BOOT → 按下并松开 RESET/EN → 松开 BOOT，再重试上传；下载模式是否成功不能仅凭 LED 判断。

### 7.3 已使用设备升级

先确认现有芯片、Flash 容量、实际分区表和应用偏移，备份必要数据。已有缓存需要保留时，优先在核对分区后仅更新应用分区。不能直接把“新板首次烧录”当作所有旧板的无损升级流程，也不要执行 `erase_flash`、文件系统格式化或改分区。

升级保持设备 ID、Token 和存储代号不变。存在新角度格式待传记录时，不降级到不认识它的旧固件或旧 PC 上传器。上传验证完成前保留该设备专用的上一版本固件。

## 8. 上线验收

### 8.1 上电、网络和时间

1. 上电后静置水平约 2 秒，观察初始化和校准。
2. 串口波特率 115200，检查 Wi-Fi 已连接、未反复重启。
3. 系统需要有效时间才能校验 HTTPS 证书。优先通过 NTP/GPS 校时。
4. 若工作台网络不能校时，可以通过物理串口发送 `@` 加当前 10 位 Unix 秒数再换行；不要照抄固定日期的时间戳。

PowerShell 可生成要发送的校时文本：

```powershell
'@' + [DateTimeOffset]::UtcNow.ToUnixTimeSeconds().ToString()
```

### 8.2 确认真正入库

观察日志中出现 `Archive committed`，以及 `inserted`、`duplicates`、`flash_pending` 等统计。HTTPS 返回成功后还会核对业务确认；只有完整确认才推进队列回收。

有历史积压时，允许 `duplicates` 大于 0，它表示安全重传，不应形成重复事件。持续上传时积压应总体下降；当前代码每批最多 64 条，完整队列与优先事件交替处理，不再使用早期“等待采集结束才补传”的流程。

### 8.3 网页验收

1. 打开实际服务器的 `/viewer/`，使用单位 ID 和 `queryToken` 登录。
2. 在设备筛选中选择新设备，确认出现新普通采样。
3. 检查 Roll/Pitch；校准有效时应有角度，“角度状态 / 历史 ADC”列已移除。
4. 检查麦克风读数和采集时间；无 GPS 定位时坐标为空是正常情况。
5. 有效 GPS 定位后确认坐标和轨迹；至少 3 个同次上电定位点才列出轨迹。
6. 通过受控动作验证设备事件识别：对应串口 `SHOT #`、优先上传确认、网页事件记录及编号。
7. 不同单位的查询凭证不应看到该设备的数据。

只看到设备下拉框有名称，不能证明上传成功；只有新记录入库才算完成接入。刷新更新后的网页可按 Ctrl+F5，重启网页服务后重新登录。

### 8.4 断网、恢复与掉电边界

1. 让设备断开 Wi-Fi，在有可用缓存空间时继续采集一段时间。
2. 恢复网络，观察缓存补传及 `flash_pending` 下降。
3. 检查补传仍保留原采集时间，事件没有重复计数。
4. 如需验证重启续传，先确认记录已写入 Flash，再重启；RAM 中尚未落盘的数据不保证掉电保留。

`Flash reclaimed` 表示已确认记录所在分段被回收。LittleFS 空间按块分配，不保证每删除一条就立即降低 `used`；不可据此强制格式化。满盘或保存失败的记录无法靠云端恢复。

## 9. 常见故障

| 现象 | 优先检查 |
|---|---|
| 上传文件提示会话过期 | 重连阿里云终端，重新上传失败文件并校验；文件上传完成不等于安装完成 |
| HTTP 401 | `archiveToken`、设备是否被停用；无凭证测试返回 401 属于正常保护 |
| HTTP 403 | Nginx 是否保留旧 Wi-Fi 来源限制；凭证是否与设备 ID 匹配 |
| HTTP 422 | 云端是否支持 `compact-v4-time`，上传字段是否符合协议 |
| HTTP 409 | 同一记录身份内容不同；检查是否错误更换身份/存储代号或修改旧记录，不能盲目清库 |
| TLS/证书失败 | 系统时间、服务器地址、证书主机名/IP、完整链与 `archiveRoot` |
| Wi-Fi 不连接 | SSID、密码、2.4 GHz 网络、工作/低功耗/休眠状态 |
| 网页 502 | `gun-viewer` 是否运行、18001 是否监听；查看服务日志 |
| 网页登录失败 | 单位 ID 与 queryToken 是否对应；网页是否读到接收端的实际数据库 |
| 网页无新数据 | 固件环境、设备身份、上传确认、设备/日期筛选；只更新网页不等于更新设备 |
| 角度为空 | 上电水平静止校准、传感器有效性；旧 ADC 记录不会自动变成角度 |
| 轨迹为空 | GPS 是否有效、同一次上电是否至少 3 个定位点、地图配置是否正确 |
| 网页仍有旧列 | 上传文件是否最新、是否替换了实际工作目录文件、是否重启并刷新 |

服务器常用诊断：

```bash
sudo systemctl status gun-receiver gun-viewer nginx --no-pager
sudo journalctl -u gun-receiver -n 80 --no-pager
sudo journalctl -u gun-viewer -n 80 --no-pager
curl --fail http://127.0.0.1:18000/healthz
curl --fail http://127.0.0.1:18001/viewer/
sudo nginx -t
```

分享日志前去掉 Token、密码、私钥和不需要暴露的位置数据。

## 10. 更新、回退和交付记录

- 仅网页样式/列变化：备份并更新网页文件，重启 `gun-viewer`。
- 上传字段/协议变化：先更新兼容的云端，再更新设备，并验证新数据。
- 接收端回退：核对 `/opt/gun-receiver/previous-release.txt` 指向的目录属于本项目，并确认协议/数据库兼容后切换 `current`；接收与网页共用 current 下的 Python 环境，因此两项服务都要检查并按需重启。
- 网页回退：恢复备份的网页与模板后重启 `gun-viewer`。
- 不把恢复旧数据库作为普通代码回退步骤，否则会丢失备份之后的新入库数据。
- 现有 CLI 的 `disable-device --id ...` 可停用上传，`disable-readers --tenant ...` 会停用该单位全部查询凭证；执行时同样使用第 5 节的真实数据库环境。

每台设备交付时记录以下信息，凭证另存私有位置，不写入此表或 Git：

| 项目 | 实际值 |
|---|---|
| 单位 ID / 设备 ID | 待填写 |
| 硬件编号 / 芯片 / Flash 容量 | 待填写 |
| 存储代号 archiveEpoch | 待填写 |
| 云端版本 / 固件版本或 SHA-256 | 待填写 |
| 上传入口 / 查询地址 | 待填写 |
| 工作时间与休眠时间（北京时间） | 待填写 |
| 首次成功入库时间 | 待填写 |
| 角度 / 事件 / GPS / 断网补传验收结果 | 待填写 |
| 配置及固件的私有保管位置 | 待填写 |

## 11. 对应源码与补充资料

- [部署打包脚本](../deploy/build_bundle.py)
- [Ubuntu 安装脚本](../deploy/ubuntu/install.sh)
- [设备注册 CLI](../cloud_backend/admin.py)
- [固件环境](../gun_detection/platformio.ini)
- [系统参数](../gun_detection/include/system_config.h)
- [HTTPS 上传实现](../gun_detection/src/archive_upload.cpp)
- [角度上传协议说明](cloud_tilt_upload.md)
- [事件补传与 Flash 回收说明](cloud_event_delivery_fix.md)

早期部署记录包含旧版批量大小、上传时机和测试时段；与当前源码不一致时，以当前源码及本文明确标注的现状为准。
