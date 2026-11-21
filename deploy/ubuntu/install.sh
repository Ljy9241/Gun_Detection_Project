#!/usr/bin/env bash
# Run from an unpacked release with root privileges. No public ingress is added.
set -euo pipefail
release_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
case "$release_dir" in
  /opt/gun-receiver/releases/*) ;;
  *) echo 'Release must be unpacked under /opt/gun-receiver/releases/.' >&2; exit 1 ;;
esac
test "$(id -u)" -eq 0
test -f "$release_dir/cloud_backend/main.py"
if test -e /opt/gun-receiver/current && ! test -L /opt/gun-receiver/current; then
  echo 'Existing current path is not a release symlink; refusing to overwrite.' >&2
  exit 1
fi
if ! getent passwd gun-receiver >/dev/null; then
  useradd --system --user-group --home-dir /var/lib/gun-receiver --shell /usr/sbin/nologin gun-receiver
fi
install -d -m 0700 -o gun-receiver -g gun-receiver /var/lib/gun-receiver
if ! test -f /etc/gun-receiver.env; then
  printf '%s\n' 'GUN_DATABASE_URL=sqlite:////var/lib/gun-receiver/receiver.db' > /etc/gun-receiver.env
  chmod 0600 /etc/gun-receiver.env
fi
# Do not overwrite a configured database URL or any existing data on upgrades.
python3 -m venv "$release_dir/.venv"
"$release_dir/.venv/bin/python" -m pip install --disable-pip-version-check --timeout 20 --retries 2 -r "$release_dir/cloud_backend/requirements.txt"
"$release_dir/.venv/bin/python" -m pip check
if test -L /opt/gun-receiver/current; then
  readlink -f /opt/gun-receiver/current > /opt/gun-receiver/previous-release.txt
fi
ln -sfn "$release_dir" /opt/gun-receiver/current
install -m 0644 "$release_dir/deploy/ubuntu/gun-receiver.service" /etc/systemd/system/gun-receiver.service
systemctl daemon-reload
systemctl enable gun-receiver.service
systemctl restart gun-receiver.service
for attempt in $(seq 1 20); do
  if curl --silent --fail http://127.0.0.1:18000/healthz; then
    printf '\nReceiver is healthy on loopback port 18000.\n'
    systemctl --no-pager --full status gun-receiver.service
    exit 0
  fi
  sleep 1
done
echo 'Health check failed. Inspect journalctl -u gun-receiver; the release pointer may need rollback.' >&2
exit 1
