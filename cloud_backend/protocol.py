"""Validate existing compact telemetry without importing the PyQt application."""
import hashlib
import json
import math
import re
from datetime import datetime, timedelta, timezone
from typing import Any, Literal

from pydantic import BaseModel, ConfigDict, Field, field_validator

IDENTIFIER = r'^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$'
FIELDS = ('sequence', 'timestamp', 'accel_x', 'accel_y', 'accel_z', 'mic_raw',
          'mic_p2p', 'mic_rms', 'latitude', 'longitude', 'bootId', 'uptimeMs', 'shotId')
BEIJING = timezone(timedelta(hours=8))


class Batch(BaseModel):
    model_config = ConfigDict(extra='forbid', strict=True, hide_input_in_errors=True)
    schemaVersion: Literal[1]
    deviceId: str = Field(pattern=IDENTIFIER)
    storageEpoch: str = Field(pattern=IDENTIFIER)
    batchId: str = Field(pattern=IDENTIFIER)
    format: Literal['compact-v1', 'compact-v2', 'compact-v2-string-u64', 'compact-v3-tilt', 'compact-v4-time']
    records: list[list[Any]] = Field(min_length=1, max_length=64)

    @field_validator('schemaVersion', mode='before')
    @classmethod
    def version_is_integer(cls, value):
        if type(value) is not int or value != 1:
            raise ValueError('schemaVersion must be integer 1')
        return value


def digest(value):
    raw = json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False)
    return hashlib.sha256(raw.encode('utf-8')).hexdigest()


def unsigned(value, bits=64, allow_string=True):
    if type(value) is int:
        number = value
    elif allow_string and isinstance(value, str) and re.fullmatch(r'[0-9]{1,20}', value):
        number = int(value)
    else:
        raise ValueError('expected an unsigned integer (64-bit values may be decimal strings)')
    if not 0 <= number < 2 ** bits:
        raise ValueError(f'integer is outside uint{bits} range')
    return number


def finite_number(value, low, high):
    if type(value) not in (int, float):
        raise ValueError('expected a finite number')
    try:
        number = float(value)
    except OverflowError as exc:
        raise ValueError('number is outside range') from exc
    if not math.isfinite(number) or not low <= number <= high:
        raise ValueError('number is outside range')
    return number


def canonical_payload(payload):
    """Hash the original capture, so adding inferred time cannot change identity."""
    original = dict(payload)
    if original.get('timestamp_source') == 'inferred':
        original['timestamp'] = original.pop('original_timestamp')
        original.pop('timestamp_source')
        original.pop('time_anchor')
    return original


def normalize(batch):
    expected = 18 if batch.format == 'compact-v4-time' else (15 if batch.format == 'compact-v3-tilt' else (13 if batch.format == 'compact-v1' else 14))
    result, identities = [], set()
    for index, row in enumerate(batch.records):
        try:
            if len(row) != expected:
                raise ValueError(f'expected {expected} compact fields')
            record = dict(zip(FIELDS + ('recordKind',), row))
            record['sequence'] = str(unsigned(record['sequence']))
            record['uptimeMs'] = str(unsigned(record['uptimeMs']))
            record['shotId'] = unsigned(record['shotId'], 32, False)
            record['recordKind'] = unsigned(record.get('recordKind', 0), 8, False)
            if record['recordKind'] not in (0, 1, 2):
                raise ValueError('unknown recordKind')
            boot = record['bootId']
            if not isinstance(boot, str) or not re.fullmatch(r'[a-fA-F0-9]{16}', boot):
                raise ValueError('bootId must contain 16 hexadecimal characters')
            record['bootId'] = boot.lower()
            tilt = unsigned(row[14], 8, False) if batch.format in ('compact-v3-tilt', 'compact-v4-time') else 0
            if tilt not in (0, 1):
                raise ValueError('unknown sensor encoding')
            if tilt:
                roll, pitch, valid = (record.pop(key) for key in ('accel_x', 'accel_y', 'accel_z'))
                if type(valid) is not bool:
                    raise ValueError('tilt validity must be boolean')
                if valid:
                    roll = finite_number(roll, -180, 180)
                    pitch = finite_number(pitch, -90, 90)
                elif roll is not None or pitch is not None:
                    raise ValueError('invalid tilt must have null angles')
                record.update(roll_deg=roll, pitch_deg=pitch, tilt_valid=valid)
            else:
                for field in ('accel_x', 'accel_y', 'accel_z'):
                    record[field] = unsigned(record[field], 16, False)
            for field in ('mic_raw', 'mic_p2p'):
                record[field] = unsigned(record[field], 16, False)
            record['mic_rms'] = finite_number(record['mic_rms'], 0, 1e12)
            lat, lon = record['latitude'], record['longitude']
            if lat is None and lon is None:
                record['gpsValid'] = False
            elif lat is None or lon is None:
                raise ValueError('latitude and longitude must both be null or both be numbers')
            else:
                record['latitude'] = finite_number(lat, -90, 90)
                record['longitude'] = finite_number(lon, -180, 180)
                record['gpsValid'] = (lat, lon) != (0, 0)
            stamp = record['timestamp']
            if not isinstance(stamp, str) or len(stamp) > 32:
                raise ValueError('invalid timestamp')
            if re.fullmatch(r'T\+[0-9]{2,}:[0-5][0-9]:[0-5][0-9]', stamp):
                occurred = None
            else:
                if not re.fullmatch(r'[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}', stamp):
                    raise ValueError('timestamp must be Beijing calendar time or T+HH:MM:SS')
                occurred = datetime.strptime(stamp, '%Y-%m-%d %H:%M:%S').replace(
                    tzinfo=BEIJING).astimezone(timezone.utc).isoformat()
            if batch.format == 'compact-v4-time' and any(value is not None for value in row[15:18]):
                raw_stamp, anchor_utc, anchor_uptime = row[15:18]
                if not isinstance(raw_stamp, str) or not re.fullmatch(r'T\+[0-9]{2,}:[0-5][0-9]:[0-5][0-9]', raw_stamp) or len(raw_stamp) > 32:
                    raise ValueError('inferred timestamp requires original T+ timestamp')
                anchor_utc, anchor_uptime = unsigned(anchor_utc), unsigned(anchor_uptime)
                uptime = int(record['uptimeMs'])
                if not 1704067200000 <= anchor_utc <= 4102444799999 or not uptime <= anchor_uptime <= anchor_utc:
                    raise ValueError('invalid time anchor')
                inferred_ms = anchor_utc - (anchor_uptime - uptime)
                if inferred_ms < 1704067200000:
                    raise ValueError('inferred time outside supported range')
                inferred = (datetime(1970, 1, 1, tzinfo=timezone.utc) +
                            timedelta(milliseconds=inferred_ms)).astimezone(BEIJING)
                if stamp != inferred.strftime('%Y-%m-%d %H:%M:%S'):
                    raise ValueError('timestamp does not match time anchor')
                record.update(original_timestamp=raw_stamp, timestamp_source='inferred',
                              time_anchor=dict(bootId=record['bootId'], utcMs=str(anchor_utc),
                                               uptimeMs=str(anchor_uptime)))
            identity = record['bootId'], record['sequence']
            if identity in identities:
                raise ValueError('duplicate record identity inside batch')
            identities.add(identity)
            kind = 'shot' if record['shotId'] else ('gps' if record['recordKind'] == 2 else 'sensor')
            result.append({'payload': record, 'occurred_at': occurred, 'kind': kind,
                           'payload_hash': digest(canonical_payload(record))})
        except (ValueError, TypeError) as exc:
            raise ValueError(f'records[{index}]: {exc}') from exc
    return result
