"""Upload-mode commands and compact telemetry wire format (no GUI dependencies)."""
import json
import time

FIELDS = ('sequence', 'timestamp', 'accel_x', 'accel_y', 'accel_z', 'mic_raw',
          'mic_p2p', 'mic_rms', 'latitude', 'longitude', 'bootId', 'uptimeMs', 'shotId')


def decode_records(packet):
    records = packet['records']
    if not isinstance(records, list) or not records or len(records) > 64:
        raise ValueError('批次记录数无效')
    if packet.get('format') == 'compact-v1':
        if any(not isinstance(row, list) or len(row) != len(FIELDS) for row in records):
            raise ValueError('紧凑遥测字段数无效')
        return [dict(zip(FIELDS, row)) for row in records]
    if packet.get('format'):
        raise ValueError('不支持的遥测格式')
    return records


class TransferControl:
    def __init__(self):
        self.status = None
        self.address = None
        self.seen = 0
        self.pending = None
        self.sent = 0
        self.deadline = 0
        self.note = '等待设备连接'
        self.rate = 0
        self.previous = None

    def observe(self, packet, now=None):
        now = time.monotonic() if now is None else now
        identity = (packet.get('deviceId'), packet.get('bootId'))
        if self.status and identity == (self.status.get('deviceId'), self.status.get('bootId')):
            if (int(packet.get('uptimeMs', 0)) < int(self.status.get('uptimeMs', 0))
                    or int(packet.get('controlId', 0)) < int(self.status.get('controlId', 0))):
                return  # An old UDP status must not undo a confirmed mode switch.
        if self.status and identity != (self.status.get('deviceId'), self.status.get('bootId')):
            self.pending = None
            self.previous = None
            self.rate = 0
            self.note = '设备已重新上电，默认正常采集'
        self.status = packet
        self.address = tuple(packet['_address'])
        self.seen = now
        if self.pending and int(packet.get('controlId', 0)) == self.pending['requestId']:
            if packet.get('uploadOnly') == self.pending['uploadOnly']:
                self.pending = None
                self.previous = None
                self.note = '模式切换已确认'
        remaining = int(packet.get('uploadPending', 0))
        if self.previous and packet.get('uploadOnly'):
            old_time, old_remaining = self.previous
            if now > old_time:
                self.rate = max(0, (old_remaining - remaining) / (now - old_time))
        self.previous = (now, remaining)

    def available(self, now=None):
        now = time.monotonic() if now is None else now
        return bool(self.status and 'uploadOnly' in self.status and self.address
                    and now - self.seen < 15 and not self.pending)

    def request(self, now=None):
        now = time.monotonic() if now is None else now
        if not self.available(now):
            return
        self.pending = dict(type='set_mode', deviceId=self.status['deviceId'],
                            bootId=self.status['bootId'],
                            requestId=max(time.time_ns() // 1000, int(self.status.get('controlId', 0)) + 1),
                            uploadOnly=not self.status['uploadOnly'])
        self.sent = now - 2
        self.deadline = now + 30
        self.note = '等待设备确认切换…'

    def command(self, now=None):
        now = time.monotonic() if now is None else now
        if not self.pending:
            return None
        if now >= self.deadline:
            self.pending = None
            self.note = '切换未确认，请等待设备状态更新后重试'
            return None
        if now - self.sent < 2:
            return None
        self.sent = now
        return json.dumps(self.pending, separators=(',', ':')).encode('utf-8'), self.address
