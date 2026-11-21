import hashlib
import json
import sqlite3

from fastapi.testclient import TestClient

from cloud_backend import viewer


def test_map_requires_three_fixes_per_boot_and_scopes_units(tmp_path, monkeypatch):
    path = tmp_path / "receiver.db"
    conn = sqlite3.connect(path)
    conn.executescript("""
        CREATE TABLE tenants(id TEXT PRIMARY KEY,name TEXT);
        CREATE TABLE tenant_readers(token_hash TEXT,tenant_id TEXT,enabled INTEGER);
        CREATE TABLE devices(id TEXT,name TEXT,tenant_id TEXT);
        CREATE TABLE records(id TEXT,tenant_id TEXT,device_id TEXT,storage_epoch TEXT,
            boot_id TEXT,sequence TEXT,kind TEXT,shot_id INTEGER,uptime_ms TEXT,
            event_date TEXT,received_at TEXT,payload TEXT);
    """)
    conn.execute("INSERT INTO tenants VALUES('unit-a','甲单位')")
    conn.execute("INSERT INTO tenants VALUES('unit-b','乙单位')")
    conn.execute("INSERT INTO tenant_readers VALUES(?,?,1)",
                 (hashlib.sha256(b"password-a").hexdigest(), "unit-a"))
    conn.execute("INSERT INTO devices VALUES('a-1','设备一','unit-a')")

    def add(i, unit, device, boot, kind="gps", valid=True,
            timestamp="2026-11-18 00:00:00", received="2026-11-17T16:00:00+00:00"):
        payload = dict(timestamp=timestamp, gpsValid=valid,
                       latitude=30 + i / 1000 if valid else None,
                       longitude=120 + i / 1000 if valid else None,
                       shotId=i if kind == "shot" else 0)
        conn.execute("INSERT INTO records VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                     (str(i) + unit + boot + kind, unit, device, "epoch", boot,
                      str(i), kind, payload["shotId"], str(i * 1000),
                      "2026-11-18" if not timestamp.startswith("T+") else None,
                      received, json.dumps(payload)))

    for i in (1, 2, 3):
        add(i, "unit-a", "a-1", "boot-a")
        add(i + 10, "unit-b", "b-1", "boot-b")
    for i in (4, 5):
        add(i, "unit-a", "a-1", "boot-short")
    add(6, "unit-a", "a-1", "boot-a", valid=False)
    add(7, "unit-a", "a-1", "boot-a", kind="shot")
    add(8, "unit-a", "a-1", "boot-a", kind="sensor",
        timestamp="T+00:00:08", received="2026-11-17T19:00:00+00:00")
    add(9, "unit-a", "a-1", "boot-a", kind="sensor",
        timestamp="2026-11-18 01:00:09", received="2026-11-17T18:00:00+00:00")
    add(10, "unit-a", "a-1", "boot-a", kind="sensor",
        timestamp="2026-11-18 01:00:10", received="2026-11-17T17:00:00+00:00")
    for i in (20, 21, 22):
        add(i, "unit-a", "a-1", "boot-live", kind="sensor",
            timestamp=f"2026-11-18 02:00:{i}")
    conn.commit()
    conn.close()

    config = tmp_path / "amap_config.json"
    config.write_text(json.dumps({"key": "test-key", "securityJsCode": "test-code"}))
    monkeypatch.setenv("VIEWER_DB", str(path))
    monkeypatch.setattr(viewer, "AMAP_CONFIG", config)
    viewer.sessions.clear()
    with TestClient(viewer.app, base_url="https://example.test") as client:
        login_page = client.get("/viewer/").text
        assert "用户登录" in login_page and "用户账号" in login_page
        assert client.get("/viewer/map/sessions").status_code == 401
        assert client.post("/viewer/login", data={"unit": "unit-a", "password": "password-a"},
                           follow_redirects=False).status_code == 303
        list_page = client.get("/viewer/").text
        assert '<option value="gps"' not in list_page
        assert '<td>GPS 轨迹</td>' not in list_page
        assert '<th>接收时间（北京）</th><th>采集时间</th>' in list_page
        assert '<th>事件编号</th>' not in list_page
        assert '<td>射击事件（编号 7）</td>' in list_page
        sensor_page = client.get('/viewer/?kind=sensor').text
        assert '<th>事件编号</th>' not in sensor_page
        assert '<td>射击事件' not in sensor_page
        shot_page = client.get('/viewer/?kind=shot').text
        assert '<th>事件编号</th>' in shot_page
        assert '<td>射击事件</td>' in shot_page
        assert '<td>7</td>' in shot_page
        assert '<th>接收时间（北京）</th><th>采集时间</th>' in shot_page
        empty_page = client.get('/viewer/?kind=sensor&day=2000-01-01').text
        assert 'colspan="11"' in empty_page
        assert (list_page.index('<td>2026-11-18 01:00:10</td>')
                < list_page.index('<td>2026-11-18 01:00:09</td>')
                < list_page.index('<td>T+00:00:08</td>'))
        assert client.get("/viewer/?kind=gps", follow_redirects=False).headers["location"] == "/viewer/map"
        map_page = client.get("/viewer/map")
        assert map_page.status_code == 200 and "GPS 轨迹地图" in map_page.text
        assert "script-src" in map_page.headers["content-security-policy"]
        items = client.get("/viewer/map/sessions").json()["items"]
        assert {item["boot"]: item["count"] for item in items} == {"boot-a": 6, "boot-live": 3}
        query = {"device": "a-1", "epoch": "epoch", "boot": "boot-a"}
        track = client.get("/viewer/map/track", params=query).json()
        assert track["raw_count"] == 6 and len(track["points"]) == 6
        assert len(track["shots"]) == 1 and track["shots"][0]["shot"] == 7
        live = client.get("/viewer/map/track", params={**query, "boot": "boot-live"}).json()
        assert live["raw_count"] == 3 and len(live["points"]) == 3 and live["shots"] == []
        assert client.get("/viewer/map/track", params={**query, "boot": "boot-short"}).status_code == 404
        assert client.get("/viewer/map/track", params={"device": "b-1", "epoch": "epoch", "boot": "boot-b"}).status_code == 404
