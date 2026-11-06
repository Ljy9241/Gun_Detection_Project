import socket
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gun_wifi_monitor
from monitor_lib.discovery import discovery_reply


class DiscoveryTests(unittest.TestCase):
    def test_protocol_validation(self):
        request = b'GUN_MONITOR_DISCOVER_V1 0123456789abcdef'
        self.assertEqual(discovery_reply(request), b'GUN_MONITOR_HERE_V1 0123456789abcdef')
        for data in (request + b'\n', request + b'junk', request[:-1],
                     request.replace(b'V1', b'V2'), b'{"type":"status"}'):
            self.assertIsNone(discovery_reply(data))

    def test_receiver_restart_and_new_address(self):
        # Real receiver sockets: discovery must not be treated as telemetry.
        with tempfile.TemporaryDirectory() as directory, patch.object(
                gun_wifi_monitor, 'DATABASE_PATH', Path(directory) / 'test.db'):
            receiver = gun_wifi_monitor.UdpReceiver()
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as client:
                client.bind(('127.0.0.1', 0))
                client.settimeout(2)
                for host in ('127.0.0.1', '127.0.0.2'):
                    try:
                        receiver.start(host, 0)
                        deadline = time.monotonic() + 3
                        address = None
                        while time.monotonic() < deadline:
                            sock = receiver._socket
                            if sock is not None and sock.getsockname()[1]:
                                address = sock.getsockname()
                                break
                            time.sleep(0.01)
                        self.assertIsNotNone(address)
                        request = b'GUN_MONITOR_DISCOVER_V1 0123456789abcdef'
                        client.sendto(request, address)
                        reply, source = client.recvfrom(512)
                        self.assertEqual(reply, discovery_reply(request))
                        self.assertEqual(source, address)
                    finally:
                        receiver.stop()
                        receiver._thread.join(timeout=3)
                        self.assertFalse(receiver._thread.is_alive())


if __name__ == '__main__':
    unittest.main()
