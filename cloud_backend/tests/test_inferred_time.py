from copy import deepcopy
import json
from datetime import datetime, timezone

import pytest
from sqlalchemy import select

from cloud_backend.database import records, shots
from cloud_backend.protocol import Batch, normalize
from cloud_backend.tests.test_receiver import env, packet, upload, query
from cloud_backend import viewer


def inferred_packet(batch_id='time-1'):
    data = packet(batch_id, stamp='2026-11-15 00:30:00')
    data['format'] = 'compact-v4-time'
    # Sample at 120s; synchronize at 210s, 00:31:30 Beijing.
    utc_ms = int(datetime(2026, 11, 14, 16, 31, 30, tzinfo=timezone.utc).timestamp()) * 1000
    data['records'][0].extend([0, 'T+00:02:00', str(utc_ms), '210000'])
    return data


def test_inferred_time_upload_replay_old_format_and_event_date(env):
    data = inferred_packet()
    assert upload(env, data).json()['insertedCount'] == 1
    assert upload(env, data).json()['duplicateCount'] == 1
    # Same identity from the old protocol and same batch ID remains idempotent.
    raw = packet('time-1', stamp='T+00:02:00')
    assert upload(env, raw).json()['duplicateCount'] == 1
    rows = query(env, '/api/v1/records').json()['items']
    assert len(rows) == 1
    row = rows[0]
    assert row['event_date'] == '2026-11-15'
    assert row['occurred_at'] == '2026-11-14T16:30:00+00:00'
    assert row['payload']['timestamp_source'] == 'inferred'
    assert row['payload']['original_timestamp'] == 'T+00:02:00'
    assert '2026-11-15 00:30:00（推算）' in viewer.render_row({**row, 'payload': json.dumps(row['payload'])})
    assert query(env, '/api/v1/statistics').json()['totalShots'] == 1
    changed = deepcopy(data)
    changed['records'][0][5] += 1
    assert upload(env, changed).status_code == 409


def test_legacy_record_is_enriched_without_duplicate_shot(env):
    raw = packet('time-1', stamp='T+00:02:00')
    assert upload(env, raw).json()['insertedCount'] == 1
    data = inferred_packet()
    assert upload(env, data).json()['duplicateCount'] == 1
    with env['db'].engine.begin() as conn:
        record = conn.execute(select(records)).mappings().one()
        shot = conn.execute(select(shots)).mappings().one()
        assert record['event_date'] == shot['event_date'] == '2026-11-15'
        assert shot['payload']['timestamp_source'] == 'inferred'
    # Rebatching and later calibration do not overwrite the first stored time.
    data['batchId'] = 'another-batch'
    data['records'][0][1] = '2026-11-15 00:30:01'
    data['records'][0][16] = str(int(data['records'][0][16]) + 1000)
    assert upload(env, data).json()['duplicateCount'] == 1
    assert query(env, '/api/v1/records').json()['items'][0]['payload']['timestamp'] == '2026-11-15 00:30:00'


@pytest.mark.parametrize('index,value', [(1, '2026-11-15 00:30:02'), (15, '2026-11-15 00:00:00'),
    (16, None), (16, '1'), (16, True), (17, '119999'), (17, str(2**64-1))])
def test_invalid_time_provenance_is_rejected(index, value):
    data = inferred_packet()
    data['records'][0][index] = value
    with pytest.raises(ValueError):
        normalize(Batch(**data))


def test_v4_without_anchor_preserves_unknown_time_and_tilt():
    data = inferred_packet()
    row = data['records'][0]
    row[1] = 'T+00:02:00'
    row[2:5] = [12.3, -4.5, True]
    row[14:] = [1, None, None, None]
    item = normalize(Batch(**data))[0]
    assert item['occurred_at'] is None
    assert item['payload']['roll_deg'] == 12.3
    assert 'timestamp_source' not in item['payload']


def test_time_inference_stays_with_record_boot_and_device(env):
    data = inferred_packet()
    assert upload(env, data).status_code == 200
    other = packet('other-boot', stamp='T+00:02:00', boot='fedcba9876543210')
    assert upload(env, other).status_code == 200
    rows = query(env, '/api/v1/records').json()['items']
    assert sum(r['occurred_at'] is None for r in rows) == 1
    assert query(env, '/api/v1/records', reader='reader_b').json()['total'] == 0
