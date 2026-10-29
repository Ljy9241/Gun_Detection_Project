"""Regression tests for offline uploads, reboot separation and durable history."""
import json
import sqlite3

import unittest
import tempfile
from pathlib import Path

import trajectory_store as history
from gun_wifi_monitor import UdpReceiver


def database(path=':memory:'):
    db = sqlite3.connect(path)
    db.executescript('''CREATE TABLE IF NOT EXISTS telemetry (
        timestamp TEXT,accel_x INTEGER,accel_y INTEGER,accel_z INTEGER,
        mic_raw INTEGER,mic_p2p INTEGER,mic_rms REAL,latitude REAL,longitude REAL);
        CREATE TABLE IF NOT EXISTS upload_receipts (
        device_id TEXT,sequence INTEGER,PRIMARY KEY(device_id,sequence));''')
    history.initialize(db)
    return db


def record(sequence=1, boot='boot-a', ms=1000, **values):
    return dict(sequence=sequence, bootId=boot, uptimeMs=ms,
                timestamp='T+00:00:01', latitude=31.2304, longitude=121.4737, **values)


class Socket:
    def __init__(self, db):
        self.db, self.acks = db, []

    def sendto(self, payload, address):
        assert not self.db.in_transaction  # Never ACK data that could roll back.
        self.acks.append(json.loads(payload))


def upload(db, records, sock=None):
    sock = sock or Socket(db)
    UdpReceiver._store_telemetry_batch_and_ack(
        db, sock, dict(type='telemetry_batch', deviceId='device', records=records), ('localhost', 8888))
    return sock


def test_reboot_offline_out_of_order_duplicate_and_reopen(tmp_path):
    path = tmp_path / 'history.db'
    db = database(path)
    upload(db, [record(3, 'boot-b'), record(2, ms=3000), record(1)])
    upload(db, [record(1), record(2, ms=3000)])
    assert db.execute('SELECT COUNT(*) FROM telemetry').fetchone()[0] == 3
    assert len(history.sessions(db)) == 2
    assert [p['ms'] for p in history.track(db, 'device', 'boot-a')['points']] == [1000, 3000]
    db.close()
    db = database(path)
    assert len(history.sessions(db)) == 2
    assert len(history.track(db, 'device', 'boot-a')['points']) == 2
    db.close()


def test_failed_batch_rolls_back_all_tables_and_does_not_ack():
    db = database()
    sock = Socket(db)
    with unittest.TestCase().assertRaises(ValueError):
        upload(db, [record(), record()], sock)
    assert sock.acks == []
    for table in ('telemetry', 'upload_receipts', 'track_points', 'track_sessions'):
        assert db.execute('SELECT COUNT(*) FROM ' + table).fetchone()[0] == 0


def test_live_shot_and_durable_copy_deduplicate_without_fake_sensor_row():
    db = database()
    with db:
        history.store(db, dict(type='shot', deviceId='device', bootId='boot-a', shotId=4,
                              uptimeUs=1234000, latitude=31, longitude=121, gpsValid=True))
    upload(db, [record(9, ms=1234, shotId=4)])
    upload(db, [record(9, ms=1234, shotId=4)])
    result = history.track(db, 'device', 'boot-a')
    assert len(result['shots']) == 1
    assert result['shots'][0]['ms'] == 1234
    assert db.execute('SELECT COUNT(*) FROM telemetry').fetchone()[0] == 0
    assert db.execute('SELECT source FROM track_points').fetchone()[0] == 'telemetry'


def test_invalid_fix_is_retained_as_gap(lat, lon):
    db = database()
    item = record(); item.update(latitude=lat, longitude=lon)
    upload(db, [item])
    point = history.track(db, 'device', 'boot-a')['points'][0]
    assert not point['valid'] and point['lat'] is None


def test_uncalibrated_clock_then_calibration_uses_uptime_order():
    db = database()
    a, b = record(), record(2, ms=2000)
    b['timestamp'] = '2026-10-28 12:00:00'
    upload(db, [b, a])
    assert [p['time'] for p in history.track(db, 'device', 'boot-a')['points']] == [
        'T+00:00:01', '2026-10-28 12:00:00']


def test_legacy_database_migrates_once():
    db = sqlite3.connect(':memory:')
    db.execute('CREATE TABLE telemetry(timestamp TEXT,latitude REAL,longitude REAL)')
    db.execute("INSERT INTO telemetry VALUES ('old',31,121)")
    history.initialize(db); history.initialize(db)
    assert len(history.sessions(db)) == 1
    assert len(history.track(db, '旧版设备', 'legacy')['points']) == 1


def test_display_prefers_durable_sample_and_preserves_invalid_gap():
    db = database()
    with db:
        history.store(db, dict(type='status', deviceId='device', bootId='boot-a', uptimeMs=1100,
                              gps=dict(valid=True, latitude=31, longitude=121)))
    invalid = record(2, ms=1500); invalid.update(latitude=None, longitude=None)
    upload(db, [record(), invalid, record(3, ms=3000)])
    points = history.track(db, 'device', 'boot-a')['points']
    assert [p['ms'] for p in points] == [1000, 1500, 3000]
    assert not points[1]['valid']


class HistoryTests(unittest.TestCase):
    def test_persistence(self):
        with tempfile.TemporaryDirectory() as folder:
            test_reboot_offline_out_of_order_duplicate_and_reopen(Path(folder))

    def test_rollback(self):
        test_failed_batch_rolls_back_all_tables_and_does_not_ack()

    def test_shots(self):
        test_live_shot_and_durable_copy_deduplicate_without_fake_sensor_row()

    def test_invalid_fixes(self):
        for lat, lon in [(None, None), (float('nan'), 120), (91, 120), (30, 181), (0, 0)]:
            with self.subTest(lat=lat, lon=lon):
                test_invalid_fix_is_retained_as_gap(lat, lon)

    def test_clock(self):
        test_uncalibrated_clock_then_calibration_uses_uptime_order()

    def test_migration(self):
        test_legacy_database_migrates_once()

    def test_gap(self):
        test_display_prefers_durable_sample_and_preserves_invalid_gap()


if __name__ == '__main__':
    import sys
    if '--smoke' in sys.argv:
        import os
        import ctypes
        ctypes.windll.ole32.CoInitializeEx(None, 2)
        os.environ['QT_QPA_PLATFORM'] = 'offscreen'
        os.environ['QTWEBENGINE_CHROMIUM_FLAGS'] = '--disable-gpu'
        from PyQt5.QtCore import QTimer, Qt
        from PyQt5.QtWidgets import QApplication
        import gun_wifi_monitor as monitor
        QApplication.setAttribute(Qt.AA_UseSoftwareOpenGL)
        app = QApplication([])
        with tempfile.TemporaryDirectory() as folder:
            monitor.DATABASE_PATH = Path(folder) / 'smoke.db'
            db = database(monitor.DATABASE_PATH)
            upload(db, [record(1), record(2, ms=5000), record(3, ms=6000, shotId=1)])
            db.close()
            window = monitor.MonitorWindow()
            window.tabs.setCurrentIndex(1)
            window.show()
            assert window.tabs.tabText(0) == '实时曲线'
            assert window.tabs.tabText(1) == 'GPS 地图'
            checks = []
            def inspect():
                window.trajectory_map.view.page().runJavaScript(
                    "JSON.stringify({bridge:!!bridge,map:!!map,loaded:mapLoaded,points:points.length,"
                    "shots:track?track.shots.length:0,selected,busy:requestBusy,hidden:document.hidden,"
                    "options:document.getElementById('sessions').options.length,"
                    "message:document.getElementById('message').textContent})",
                    finished)
            def finished(raw):
                state = json.loads(raw) if raw else {}
                print('Qt map smoke:', json.dumps(state, ensure_ascii=True))
                checks.append(state)
                window.close()
                app.quit()
            QTimer.singleShot(15000, inspect)
            QTimer.singleShot(5000, lambda: window.trajectory_map.view.page().runJavaScript('refresh(true)'))
            app.exec_()
            assert checks and checks[0].get('bridge') and checks[0].get('points') == 2
            assert checks[0].get('shots') == 1
    else:
        unittest.main()
