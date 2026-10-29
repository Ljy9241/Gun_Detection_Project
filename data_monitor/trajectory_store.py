"""Durable boot-scoped GPS history. Called in the receiver's SQLite transaction."""
import math
import time


def initialize(database):
    database.executescript("""
        CREATE TABLE IF NOT EXISTS track_sessions (
            device TEXT NOT NULL, boot TEXT NOT NULL, first_seen REAL NOT NULL,
            live_seen REAL, revision INTEGER NOT NULL DEFAULT 0, PRIMARY KEY(device, boot));
        CREATE TABLE IF NOT EXISTS track_points (
            id INTEGER PRIMARY KEY, device TEXT NOT NULL, boot TEXT NOT NULL,
            point_key TEXT NOT NULL, kind TEXT NOT NULL, uptime_ms INTEGER NOT NULL,
            timestamp TEXT, latitude REAL, longitude REAL, valid INTEGER NOT NULL,
            source TEXT NOT NULL, shot_id INTEGER NOT NULL DEFAULT 0,
            UNIQUE(device, boot, point_key));
        CREATE INDEX IF NOT EXISTS track_order ON track_points(device, boot, uptime_ms);
        CREATE TABLE IF NOT EXISTS track_meta (name TEXT PRIMARY KEY);
    """)
    if 'revision' not in {r[1] for r in database.execute('PRAGMA table_info(track_sessions)')}:
        database.execute('ALTER TABLE track_sessions ADD COLUMN revision INTEGER NOT NULL DEFAULT 0')
    database.executescript("""
        CREATE TRIGGER IF NOT EXISTS track_insert AFTER INSERT ON track_points BEGIN
            UPDATE track_sessions SET revision=revision+1 WHERE device=new.device AND boot=new.boot;
        END;
        CREATE TRIGGER IF NOT EXISTS track_update AFTER UPDATE ON track_points BEGIN
            UPDATE track_sessions SET revision=revision+1 WHERE device=new.device AND boot=new.boot;
        END;
    """)
    # Existing rows have neither device nor boot identity. Preserve them explicitly.
    if not database.execute("SELECT 1 FROM track_meta WHERE name='legacy_import'").fetchone():
        if database.execute("SELECT 1 FROM sqlite_master WHERE name='telemetry'").fetchone():
            database.execute("""INSERT OR IGNORE INTO track_points
                (device,boot,point_key,kind,uptime_ms,timestamp,latitude,longitude,valid,source)
                SELECT '旧版设备','legacy','import:'||rowid,'gps',rowid*1000,timestamp,
                latitude,longitude,
                CASE WHEN latitude BETWEEN -90 AND 90 AND longitude BETWEEN -180 AND 180
                    AND NOT (latitude=0 AND longitude=0) THEN 1 ELSE 0 END,'legacy'
                FROM telemetry""")
            database.execute("""INSERT OR IGNORE INTO track_sessions(device,boot,first_seen)
                SELECT DISTINCT device,boot,? FROM track_points""", (time.time(),))
        database.execute("INSERT INTO track_meta VALUES ('legacy_import')")
    database.commit()


def boot_id(packet):
    boot = str(packet.get('bootId') or 'legacy')
    return 'legacy' if boot == '0000000000000000' else boot


def coordinates(latitude, longitude, valid=True):
    try:
        lat, lon = float(latitude), float(longitude)
        if (valid and math.isfinite(lat) and math.isfinite(lon)
                and -90 <= lat <= 90 and -180 <= lon <= 180 and (lat != 0 or lon != 0)):
            return lat, lon, 1
    except (TypeError, ValueError):
        pass
    return None, None, 0


def store(database, packet, record=None):
    record = packet if record is None else record
    device = str(packet.get('deviceId', '未知设备'))
    boot = boot_id(record)
    kind = packet.get('type')
    live = kind in ('status', 'shot')
    shot_id = int(record.get('shotId') or 0)
    point_kind = 'shot' if shot_id else 'gps'
    if kind == 'status':
        gps = packet.get('gps') or {}
        lat, lon, valid = coordinates(gps.get('latitude'), gps.get('longitude'), gps.get('valid', False))
    else:
        lat, lon, valid = coordinates(record.get('latitude'), record.get('longitude'),
                                      record.get('gpsValid', True))
    uptime = int(record.get('uptimeMs') or int(record.get('uptimeUs', 0)) // 1000)
    sequence = int(record.get('sequence', 0))
    if boot == 'legacy':
        uptime = sequence or uptime
    key = f'shot:{shot_id}' if shot_id else (f'seq:{sequence}' if not live else f'live:{uptime}')
    source = 'live' if live else 'telemetry'
    now = time.time()
    database.execute("""INSERT INTO track_sessions(device,boot,first_seen,live_seen) VALUES(?,?,?,?)
        ON CONFLICT(device,boot) DO UPDATE SET live_seen=COALESCE(excluded.live_seen,live_seen)""",
        (device, boot, now, now if live else None))
    # A durable shot replaces its live copy, retaining a stable identity across retries.
    database.execute("""INSERT INTO track_points
        (device,boot,point_key,kind,uptime_ms,timestamp,latitude,longitude,valid,source,shot_id)
        VALUES(?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(device,boot,point_key) DO UPDATE SET
        timestamp=excluded.timestamp,latitude=excluded.latitude,longitude=excluded.longitude,
        valid=excluded.valid,source=excluded.source
        WHERE track_points.source='live' AND excluded.source='telemetry'""",
        (device, boot, key, point_kind, uptime, record.get('timestamp'), lat, lon, valid, source, shot_id))


def sessions(database):
    rows = database.execute("""SELECT s.device,s.boot,s.first_seen,s.live_seen,
        (SELECT timestamp FROM track_points p WHERE p.device=s.device AND p.boot=s.boot
            WHERE p.valid=1 ORDER BY p.uptime_ms LIMIT 1),
        (SELECT uptime_ms FROM track_points p WHERE p.device=s.device AND p.boot=s.boot
            AND p.valid=1 ORDER BY p.uptime_ms LIMIT 1)
        FROM track_sessions s WHERE EXISTS (
            SELECT 1 FROM track_points p WHERE p.device=s.device AND p.boot=s.boot AND p.valid=1)
        ORDER BY COALESCE(s.live_seen,s.first_seen) DESC""").fetchall()
    return [dict(device=r[0], boot=r[1], first_seen=r[2], live_seen=r[3],
                 timestamp=r[4], start_ms=r[5]) for r in rows]


def track(database, device, boot):
    count, last_id, start, end = database.execute("""SELECT COUNT(*),MAX(id),MIN(uptime_ms),MAX(uptime_ms)
        FROM track_points WHERE device=? AND boot=?""", (device, boot)).fetchone()
    # Keep the desktop responsive for long sessions. Raw records are never deleted.
    bucket_ms = max(1000, math.ceil(((end or 0) - (start or 0) + 1) / 10000))
    rows = database.execute("""SELECT uptime_ms,timestamp,latitude,longitude,valid FROM (
        SELECT *,ROW_NUMBER() OVER (
            PARTITION BY uptime_ms / ?, valid ORDER BY
            CASE source WHEN 'telemetry' THEN 0 ELSE 1 END,uptime_ms DESC,id DESC) AS n
        FROM track_points WHERE device=? AND boot=? AND kind='gps')
        WHERE n=1 ORDER BY uptime_ms""", (bucket_ms, device, boot)).fetchall()
    # Always retain the actual first and latest fix, even within a display bucket.
    for order in ('ASC', 'DESC'):
        endpoint = database.execute("""SELECT uptime_ms,timestamp,latitude,longitude,valid
            FROM track_points WHERE device=? AND boot=? AND kind='gps' AND valid=1
            ORDER BY uptime_ms """ + order + ' LIMIT 1', (device, boot)).fetchone()
        if endpoint and endpoint not in rows:
            rows.append(endpoint)
    rows.sort(key=lambda row: row[0])
    shots = database.execute("""SELECT uptime_ms,timestamp,latitude,longitude,valid,shot_id
        FROM track_points WHERE device=? AND boot=? AND kind='shot' ORDER BY uptime_ms""",
        (device, boot)).fetchall()
    def point(row):
        return dict(ms=row[0], time=row[1] or '未校时', lat=row[2] if row[4] else None,
                    lon=row[3] if row[4] else None, valid=bool(row[4]))
    revision = database.execute('SELECT revision FROM track_sessions WHERE device=? AND boot=?',
                                (device, boot)).fetchone()
    return dict(device=device, boot=boot, revision=[count, last_id, revision[0] if revision else 0], bucket_ms=bucket_ms,
                points=[point(r) for r in rows], shots=[dict(point(r), shot=r[5]) for r in shots])
