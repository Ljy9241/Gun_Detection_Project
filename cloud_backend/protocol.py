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
    format: Literal['compact-v1', 'compact-v2', 'compact-v2-string-u64']
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


def normalize(batch):
    expected = 13 if batch.format == 'compact-v1' else 14
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
            for field in ('accel_x', 'accel_y', 'accel_z', 'mic_raw', 'mic_p2p'):
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
            identity = record['bootId'], record['sequence']
            if identity in identities:
                raise ValueError('duplicate record identity inside batch')
            identities.add(identity)
            kind = 'shot' if record['shotId'] else ('gps' if record['recordKind'] == 2 else 'sensor')
            result.append({'payload': record, 'occurred_at': occurred, 'kind': kind,
                           'payload_hash': digest(record)})
        except (ValueError, TypeError) as exc:
            raise ValueError(f'records[{index}]: {exc}') from exc
    return result
