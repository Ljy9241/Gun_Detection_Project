import json
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from monitor_lib.transfer_control import TransferControl, FIELDS, decode_records
from test_trajectory import database, record, Socket
from gun_wifi_monitor import UdpReceiver
from monitor_lib import trajectory_store


class TransferTests(unittest.TestCase):
    def status(self, **extra):
        return dict(type='status', deviceId='device', bootId='boot-a', uptimeMs=1000,
                    _address=['127.0.0.1', 8889], **extra)

    def test_retry_confirm_and_reboot(self):
        control = TransferControl()
        control.observe(self.status(uploadOnly=False), now=10)
        control.request(now=10)
        command, address = control.command(now=10)
        request = json.loads(command)
        self.assertTrue(request['uploadOnly'])
        self.assertEqual(address, ('127.0.0.1', 8889))
        self.assertIsNone(control.command(now=11))
        self.assertEqual(control.command(now=12)[0], command)
        self.assertFalse(control.status['uploadOnly'])  # Do not claim early success.
        control.observe(self.status(uploadOnly=True, controlId=request['requestId'], uploadPending=20), now=13)
        self.assertIsNone(control.pending)
        control.observe(self.status(uploadOnly=False, controlId=0), now=13.5)
        self.assertTrue(control.status['uploadOnly'])
        control.request(now=14)
        self.assertFalse(json.loads(control.command(now=14)[0])['uploadOnly'])
        boot = self.status(uploadOnly=False)
        boot['bootId'] = 'boot-b'
        control.observe(boot, now=15)
        self.assertIsNone(control.pending)
        self.assertFalse(control.status['uploadOnly'])

    def test_timeout_and_unsupported_firmware(self):
        control = TransferControl()
        control.observe(self.status(), now=10)
        self.assertFalse(control.available(now=10))
        control.observe(self.status(uploadOnly=False), now=11)
        control.request(now=11)
        self.assertIsNone(control.command(now=42))
        self.assertIsNone(control.pending)
        self.assertFalse(control.available(now=42))

    def test_transfer_status_creates_no_points(self):
        db = database()
        with db:
            trajectory_store.store(db, self.status(uploadOnly=True, gps=dict(valid=True, latitude=31, longitude=121)))
        self.assertEqual(db.execute('SELECT COUNT(*) FROM track_points').fetchone()[0], 0)
        self.assertEqual(trajectory_store.sessions(db), [])
        db.close()

    def test_compact_transaction_duplicate_and_shot(self):
        db = database()
        records = [record(i, ms=i*1000, shotId=7 if i == 7 else 0) for i in range(1, 8)]
        packet = dict(type='telemetry_batch', deviceId='device', format='compact-v1',
                      records=[[r.get(f, 0) for f in FIELDS] for r in records])
        sock = Socket(db)
        for _ in range(2):
            UdpReceiver._store_telemetry_batch_and_ack(db, sock, packet, ('localhost', 8888))
        self.assertEqual(sock.acks[0]['sequences'], list(range(1, 8)))
        self.assertEqual(db.execute('SELECT COUNT(*) FROM telemetry').fetchone()[0], 6)
        self.assertEqual(db.execute('SELECT COUNT(*) FROM track_points').fetchone()[0], 7)
        packet['records'][-1].pop()
        with self.assertRaises(ValueError):
            UdpReceiver._store_telemetry_batch_and_ack(db, sock, packet, ('localhost', 8888))
        self.assertEqual(len(sock.acks), 2)
        db.close()

    def test_compact_payload_fits_one_datagram(self):
        row = [2**64-1, '2026-10-29 23:59:59', 65535, 65535, 65535, 65535, 65535,
               4095.0, -90.1234567, -180.1234567, 'ffffffffffffffff', 2**64-1, 2**32-1]
        packet = dict(type='telemetry_batch', deviceId='ESP32-S3-GUN-001',
                      format='compact-v1', records=[row]*7)
        self.assertLess(len(json.dumps(packet, separators=(',', ':')).encode()), 1472)
        self.assertEqual(len(decode_records(packet)), 7)


if __name__ == '__main__':
    unittest.main()
