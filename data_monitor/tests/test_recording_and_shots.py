import json
import sqlite3
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gun_wifi_monitor import UdpReceiver
from monitor_lib import shot_store, trajectory_store
from monitor_lib.transfer_control import FIELDS_V2, decode_records
from test_trajectory import database, record, Socket


class RecordingAndShotTests(unittest.TestCase):
    def setUp(self):
        self.db = database()
        shot_store.initialize(self.db)
        self.sock = Socket(self.db)

    def tearDown(self):
        self.db.close()

    def upload(self, rows):
        packet = dict(type='telemetry_batch', deviceId='device', format='compact-v2',
                      records=[[row.get(f, 0) for f in FIELDS_V2] for row in rows])
        UdpReceiver._store_telemetry_batch_and_ack(self.db, self.sock, packet,
                                                 ('localhost', 8888))
        return packet

    def test_independent_gps_and_sensor_rows(self):
        rows = [record(i, ms=i * 1000, recordKind=2) for i in range(1, 11)]
        rows.append(record(11, ms=10000, recordKind=1, mic_raw=1960, accel_x=2000))
        for i in range(0, len(rows), 7):
            self.upload(rows[i:i + 7])
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM telemetry').fetchone()[0], 1)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM track_points').fetchone()[0], 10)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM shots').fetchone()[0], 0)
        self.assertEqual(self.db.execute('SELECT mic_raw,accel_x FROM telemetry').fetchone(), (1960, 2000))
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM upload_receipts').fetchone()[0], 11)

    def test_live_durable_retry_and_boot_deduplication(self):
        live = dict(type='shot', deviceId='device', bootId='boot-a', shotId=7,
                    uptimeUs=1234000, timestamp='T+00:00:01',
                    latitude=31, longitude=121, gpsValid=True)
        with self.db:
            trajectory_store.store(self.db, live)
            trajectory_store.store(self.db, live)
        durable = record(99, ms=1234, shotId=7, recordKind=0)
        self.upload([durable]); self.upload([durable])
        with self.db:
            trajectory_store.store(self.db, live)  # Old live packet cannot overwrite durable copy.
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM shots').fetchone()[0], 1)
        self.assertEqual(self.db.execute("SELECT source FROM track_points WHERE kind='shot'").fetchone(), ('telemetry',))
        self.assertEqual(self.db.execute('SELECT latitude,longitude FROM shots').fetchone(),
                         (durable['latitude'], durable['longitude']))
        self.upload([record(100, boot='boot-b', shotId=7, recordKind=0)])
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM shots').fetchone()[0], 2)
        self.assertEqual(self.db.execute('SELECT shot_id FROM shots ORDER BY rowid').fetchall(), [(7,), (7,)])
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM telemetry').fetchone()[0], 0)

    def test_invalid_gps_shot_and_relative_timestamp_are_preserved(self):
        row = record(1, shotId=1, recordKind=0)
        row.update(latitude=None, longitude=None)
        self.upload([row])
        self.assertEqual(self.db.execute('SELECT timestamp,latitude,longitude FROM shots').fetchone(),
                         ('T+00:00:01', None, None))

    def test_shot_write_failure_rolls_back_and_never_acks(self):
        self.db.execute("CREATE TRIGGER fail_shot BEFORE INSERT ON shots BEGIN SELECT RAISE(ABORT,'test'); END")
        with self.assertRaises(sqlite3.DatabaseError):
            self.upload([record(1, recordKind=1), record(2, shotId=1, recordKind=0)])
        self.assertEqual(self.sock.acks, [])
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM upload_receipts').fetchone()[0], 0)
        self.assertEqual(self.db.execute('SELECT COUNT(*) FROM telemetry').fetchone()[0], 0)

    def test_four_column_table_backfill_and_reopen_original_database(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'gun_telemetry.db'
            db = database(path)
            with db:
                trajectory_store.store(db, dict(type='shot', deviceId='device', bootId='boot-a',
                    shotId=4, timestamp='T+00:00:01', uptimeMs=1000,
                    latitude=31, longitude=121, gpsValid=True))
            shot_store.initialize(db)
            shot_store.initialize(db)
            db.close()
            db = sqlite3.connect(path)
            self.assertEqual([r[1] for r in db.execute('PRAGMA table_info(shots)')],
                             ['shot_id', 'timestamp', 'latitude', 'longitude'])
            self.assertEqual(db.execute('SELECT * FROM shots').fetchall(),
                             [(4, 'T+00:00:01', 31.0, 121.0)])
            db.close()

    def test_previous_separate_database_migrates_idempotently(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'gun_shots.db'
            old = sqlite3.connect(path)
            old.execute("""CREATE TABLE shots(device_id,boot_id,shot_id,timestamp,uptime_ms,
                            latitude,longitude,gps_valid,sequence,source)""")
            old.executemany('INSERT INTO shots VALUES(?,?,?,?,?,?,?,?,?,?)', [
                ('device','boot-a',7,'T+00:00:01',1000,31,121,1,10,'telemetry'),
                ('device','boot-b',7,'T+00:00:02',2000,None,None,0,11,'telemetry')])
            old.commit(); old.close()
            shot_store.import_legacy_database(self.db, path)
            shot_store.import_legacy_database(self.db, path)
            rows = self.db.execute('SELECT * FROM shots ORDER BY rowid').fetchall()
            self.assertEqual(len(rows), 2)
            self.assertEqual([row[0] for row in rows], [7, 7])
            self.assertEqual(rows[0][1:], ('T+00:00:01',31.0,121.0))
            self.assertEqual(rows[1][1:], ('T+00:00:02',None,None))
            old = sqlite3.connect(path)
            self.assertEqual(old.execute('SELECT COUNT(*) FROM shots').fetchone()[0], 2)
            old.close()

    def test_global_ids_migrate_to_per_boot_shot_counters(self):
        db = database()
        with db:
            for boot in ('boot-a', 'boot-b'):
                trajectory_store.store(db, dict(type='shot', deviceId='device', bootId=boot,
                    shotId=1, timestamp='T+00:00:01', uptimeMs=1000,
                    latitude=31, longitude=121, gpsValid=True))
        db.execute('CREATE TABLE shots(shot_id INTEGER PRIMARY KEY,timestamp TEXT,latitude REAL,longitude REAL)')
        db.execute("INSERT INTO shots SELECT id,timestamp,latitude,longitude FROM track_points WHERE kind='shot'")
        db.commit()
        shot_store.initialize(db)
        shot_store.initialize(db)
        self.assertEqual(db.execute('SELECT shot_id FROM shots ORDER BY rowid').fetchall(), [(1,), (1,)])
        self.assertEqual(len(db.execute('PRAGMA table_info(shots)').fetchall()), 4)
        self.assertFalse(db.execute('PRAGMA table_info(shots)').fetchone()[5])
        db.close()

    def test_compact_v2_size_validation_and_legacy_format(self):
        row = [2**64-1, '2026-10-29 23:59:59', 65535, 65535, 65535, 65535, 65535,
               4095.0, -90.1234567, -180.1234567, 'ffffffffffffffff', 2**64-1, 2**32-1, 2]
        packet = dict(type='telemetry_batch', deviceId='ESP32-S3-GUN-001', format='compact-v2', records=[row]*7)
        self.assertLess(len(json.dumps(packet, separators=(',', ':')).encode()), 1472)
        self.assertEqual(decode_records(packet)[0]['recordKind'], 2)
        packet['records'] = [row[:-1]]
        with self.assertRaises(ValueError): decode_records(packet)
        packet['format'] = 'compact-v1'
        self.assertNotIn('recordKind', decode_records(packet)[0])


if __name__ == '__main__':
    unittest.main()
