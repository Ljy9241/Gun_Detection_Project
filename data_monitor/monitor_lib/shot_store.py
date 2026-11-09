"""Four-column shots table in the existing telemetry database.

The canonical track_points identity handles device/boot deduplication. Triggers
use its stable hidden rowid for deduplication, while displaying the device shot counter.
"""
import sqlite3
from pathlib import Path
from . import trajectory_store


def initialize(database):
    # A hidden rowid retains the canonical identity. Visible shot_id is the
    # device's per-boot counter, so values may repeat after a device restart.
    database.execute('SAVEPOINT shots_schema')
    try:
        database.execute('DROP TRIGGER IF EXISTS shots_track_insert')
        database.execute('DROP TRIGGER IF EXISTS shots_track_update')
        columns = database.execute('PRAGMA table_info(shots)').fetchall()
        if any(column[1] == 'shot_id' and column[5] for column in columns):
            # Migrate the previous global-ID schema without dropping any rows.
            database.execute("""CREATE TABLE shots_counter_migration (
                shot_id INTEGER NOT NULL, timestamp TEXT, latitude REAL, longitude REAL)""")
            database.execute("""INSERT INTO shots_counter_migration
                (rowid,shot_id,timestamp,latitude,longitude)
                SELECT s.shot_id,COALESCE(p.shot_id,s.shot_id),s.timestamp,s.latitude,s.longitude
                FROM shots s LEFT JOIN track_points p ON p.id=s.shot_id AND p.kind='shot'""")
            database.execute('DROP TABLE shots')
            database.execute('ALTER TABLE shots_counter_migration RENAME TO shots')
        else:
            database.execute("""CREATE TABLE IF NOT EXISTS shots (
                shot_id INTEGER NOT NULL, timestamp TEXT, latitude REAL, longitude REAL)""")
        database.execute('CREATE INDEX IF NOT EXISTS shot_time ON shots(timestamp)')
        for event, trigger in [('INSERT', 'shots_track_insert'), ('UPDATE', 'shots_track_update')]:
            database.execute(f"""CREATE TRIGGER {trigger} AFTER {event} ON track_points
                WHEN new.kind='shot' AND new.shot_id>0 BEGIN
                    INSERT INTO shots(rowid,shot_id,timestamp,latitude,longitude)
                    VALUES(new.id,new.shot_id,new.timestamp,
                           CASE WHEN new.valid THEN new.latitude END,
                           CASE WHEN new.valid THEN new.longitude END)
                    ON CONFLICT(rowid) DO UPDATE SET
                    shot_id=excluded.shot_id,timestamp=excluded.timestamp,
                    latitude=excluded.latitude,longitude=excluded.longitude;
                END""")
        database.execute("""INSERT INTO shots(rowid,shot_id,timestamp,latitude,longitude)
            SELECT id,shot_id,timestamp,CASE WHEN valid THEN latitude END,
                   CASE WHEN valid THEN longitude END
            FROM track_points WHERE kind='shot' AND shot_id>0
            ON CONFLICT(rowid) DO UPDATE SET
            shot_id=excluded.shot_id,timestamp=excluded.timestamp,
            latitude=excluded.latitude,longitude=excluded.longitude""")
        database.execute('RELEASE SAVEPOINT shots_schema')
    except Exception:
        database.execute('ROLLBACK TO SAVEPOINT shots_schema')
        database.execute('RELEASE SAVEPOINT shots_schema')
        raise


def import_legacy_database(database, path):
    """Merge the previously created independent DB without modifying that backup."""
    path = Path(path)
    if not path.exists():
        return
    legacy = sqlite3.connect(path.resolve().as_uri() + '?mode=ro', uri=True, timeout=10)
    try:
        if not legacy.execute("SELECT 1 FROM sqlite_master WHERE type='table' AND name='shots'").fetchone():
            return
        with database:
            for row in legacy.execute("""SELECT device_id,boot_id,shot_id,timestamp,uptime_ms,
                    latitude,longitude,gps_valid,sequence,source FROM shots"""):
                device, boot, shot, stamp, uptime, lat, lon, valid, sequence, source = row
                packet = dict(type='shot' if source == 'live' else 'telemetry_batch', deviceId=device)
                record = dict(bootId=boot, shotId=shot, timestamp=stamp, uptimeMs=uptime,
                              latitude=lat, longitude=lon, gpsValid=bool(valid), sequence=sequence or 0)
                trajectory_store.store(database, packet, record)
    finally:
        legacy.close()
