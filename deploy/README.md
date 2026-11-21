# Ubuntu 私有部署

此部署先在服务器内完成接收服务联调，仅监听 `127.0.0.1:18000`。不会修改已有 8080/8081 服务、自动放开安全组或创建付费云资源。公网 HTTPS、域名和正式数据库另行配置。

构建（项目根目录）：

```powershell
cloud_backend/.venv/Scripts/python.exe deploy/build_bundle.py --release receiver-20261117-01
```

输出位于 `output/deployment`，只包含接收服务源码、运行依赖、Linux 安装文件及校验清单；不包含设备/单位凭据、本地数据库、固件、GPS 历史、虚拟环境或模拟设备。

在 Ubuntu 服务器上，核对归档 SHA-256 后解包到 `/opt/gun-receiver/releases/<版本>`，再执行该目录中的 `bash deploy/ubuntu/install.sh`。需要现有 Python 3、venv、curl 和 systemd；如缺少 venv，通过系统官方软件源安装对应包。安装失败时先检查日志，不覆盖其他服务。

服务进程使用无登录 shell 的 `gun-receiver` 系统用户，源码保留 root 所有权，数据目录 `/var/lib/gun-receiver` 权限 0700，配置文件 `/etc/gun-receiver.env` 权限 0600。单进程内存上限 256 MiB，超限可能重启；上线后需观测真实负载再调整。

安装脚本默认使用独立 SQLite 数据库并保留既有配置，不将此内测部署等同 RDS 长期生产部署。系统服务自动启动和故障重启；`systemctl status gun-receiver` 查看状态，`journalctl -u gun-receiver` 查看日志。

升级只切换 `current` 符号链接，数据始终位于版本目录之外。旧版本路径记录在 `/opt/gun-receiver/previous-release.txt`。回退时先验证旧目录确实属于本项目、核查数据库模式兼容，再将 current 指向旧版本并重启本服务；不要重启整台服务器或删除数据库。

在服务器上创建单位/设备时，通过环境文件设置数据库地址，以 `gun-receiver` 用户运行 `cloud_backend.admin`，避免误在版本目录内另建数据库。凭据仅本地创建时显示；后台没有默认账号。

独立 HTTP 验证工具为 `deploy/verify_receiver.py`，使用接收服务的 Python 环境执行，`--source` 指定实际发布目录。它只创建临时测试数据库、临时进程和临时身份，检查上传、响应丢失重传、内容冲突、单位隔离及重启续传；不向运行数据库写测试事件，也不是模拟设备服务。可以在本地先运行：

```powershell
cloud_backend/.venv/Scripts/python.exe deploy/verify_receiver.py
```

公网开放前需要确定实际域名、TLS 证书、必要备案和请求限流。只由反向代理转发到回环端口，不直接将 18000 暴露给互联网。
