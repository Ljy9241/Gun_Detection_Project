"""Receiver integration tests; temporary fixtures, not runnable device simulators."""
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import event, func, select
from sqlalchemy.exc import OperationalError

from cloud_backend.config import Settings
from cloud_backend.database import devices, outbox, records
from cloud_backend.main import create_app
from cloud_backend.service import provision_device, provision_tenant


def auth(token):
    return {'Authorization': f'Bearer {token}'}


def packet(batch_id='batch-1', sequence='1', shot=1, stamp='2026-11-15 00:30:00', boot='0123456789abcdef'):
    return dict(schemaVersion=1, deviceId='device-a', storageEpoch='epoch-1', batchId=batch_id,
        format='compact-v2-string-u64', records=[
            [sequence, stamp, 1, 2, 3, 4, 5, 0.1, 31.0, 121.0, boot, '120000', shot, 0]])


@pytest.fixture
def env(tmp_path):
    settings = Settings(f'sqlite:///{(tmp_path / "receiver.db").as_posix()}')
    app = create_app(settings)
    with TestClient(app) as client:
        db = app.state.database
        reader_a = provision_tenant(db, 'unit-a', '单位甲')
        reader_b = provision_tenant(db, 'unit-b', '单位乙')
        token_a = provision_device(db, 'unit-a', 'device-a', '设备甲')
        token_b = provision_device(db, 'unit-b', 'device-b', '设备乙')
        yield dict(app=app, client=client, db=db, settings=settings,
                   a=token_a, b=token_b, reader_a=reader_a, reader_b=reader_b)


def upload(env, payload, token=None):
    return env['client'].post('/device/v1/batches', json=payload, headers=auth(token or env['a']))


def query(env, path, reader='reader_a', **kwargs):
    return env['client'].get(path, headers=auth(env[reader]), **kwargs)


def test_upload_replay_and_rebatched_retry(env):
    first = upload(env, packet()).json()
    assert first['committed'] is True
    assert first['insertedCount'] == 1
    assert first['records'] == [{'bootId': '0123456789abcdef', 'sequence': '1'}]
    replay = upload(env, packet()).json()
    assert (replay['insertedCount'], replay['duplicateCount']) == (0, 1)
    retry = upload(env, packet('after-reboot')).json()
    assert (retry['insertedCount'], retry['duplicateCount']) == (0, 1)
    assert query(env, '/api/v1/statistics').json()['totalShots'] == 1
    with env['db'].engine.begin() as connection:
        assert connection.execute(select(func.count()).select_from(outbox)).scalar_one() == 1


def test_data_survives_service_restart_and_retries(env):
    assert upload(env, packet()).status_code == 200
    restarted = create_app(env['settings'])
    with TestClient(restarted) as client:
        result = client.post('/device/v1/batches', json=packet('restarted'), headers=auth(env['a']))
        assert result.json()['duplicateCount'] == 1
        assert client.get('/api/v1/shots', headers=auth(env['reader_a'])).json()['total'] == 1


def test_64_record_firmware_batch_and_receipt_fit_buffers(env):
    payload = packet('q-64', shot=0)
    template = payload['records'][0]
    payload['records'] = []
    for i in range(64):
        record = deepcopy(template)
        record[0] = str(2**64 - 64 + i)
        record[2:7] = [65535] * 5
        record[7] = 1e12
        record[8:10] = [-90.0, -180.0]
        record[11] = str(2**64 - 1)
        record[12] = i + 1
        payload['records'].append(record)
    response = upload(env, payload)
    assert response.status_code == 200
    ack = response.json()
    assert ack['committed'] and ack['acceptedCount'] == ack['insertedCount'] == 64
    assert len(ack['records']) == 64 and len(response.content) < 6144
    assert len(response.request.content) < 16384
    retry = upload(env, payload).json()
    assert (retry['insertedCount'], retry['duplicateCount']) == (0, 64)
    assert query(env, '/api/v1/shots').json()['total'] == 64
    oversized = deepcopy(payload)
    oversized['records'].append(deepcopy(template))
    assert upload(env, oversized).status_code == 422


def test_priority_shot_then_fifo_reclaims_without_duplicate_shot(env):
    # A live shot may arrive before older sensor/GPS rows and without a GPS fix.
    shot = packet('s-priority', sequence='3')
    shot['records'][0][8:10] = [None, None]
    ack = upload(env, shot).json()
    assert ack['committed'] and ack['acceptedCount'] == 1
    assert query(env, '/api/v1/shots').json()['total'] == 1

    fifo = packet('q-fifo', sequence='1', shot=0)
    fifo['records'][0][13] = 1
    gps = deepcopy(fifo['records'][0])
    gps[0], gps[13] = '2', 2
    fifo['records'].extend([gps, deepcopy(shot['records'][0])])
    ack = upload(env, fifo).json()
    assert ack['committed'] is True
    assert (ack['acceptedCount'], ack['insertedCount'], ack['duplicateCount']) == (3, 2, 1)
    assert [r['sequence'] for r in ack['records']] == ['1', '2', '3']
    # Lost FIFO receipt and reboot: the same records remain safe to retry.
    retry = upload(env, fifo).json()
    assert (retry['insertedCount'], retry['duplicateCount']) == (0, 3)
    stats = query(env, '/api/v1/statistics').json()
    assert (stats['totalRecords'], stats['totalShots']) == (3, 1)


def test_conflicting_record_rolls_back_whole_batch(env):
    upload(env, packet())
    payload = packet('mixed', sequence='2', shot=0)
    bad = deepcopy(packet()['records'][0])
    bad[2] = 99
    payload['records'].append(bad)
    assert upload(env, payload).status_code == 409
    assert query(env, '/api/v1/records').json()['total'] == 1
    retry = packet('mixed', sequence='2', shot=0)
    assert upload(env, retry).json()['insertedCount'] == 1


def test_batch_id_cannot_be_reused_with_other_records(env):
    upload(env, packet())
    assert upload(env, packet(sequence='2')).status_code == 409
    assert query(env, '/api/v1/statistics').json()['totalRecords'] == 1


def test_shot_identity_counts_once_even_with_another_sequence(env):
    upload(env, packet())
    assert upload(env, packet('other-sequence', sequence='2')).status_code == 200
    stats = query(env, '/api/v1/statistics').json()
    assert (stats['totalRecords'], stats['totalShots']) == (2, 1)
    conflict = packet('shot-conflict', sequence='3')
    conflict['records'][0][8] = 32.0
    assert upload(env, conflict).status_code == 409
    assert query(env, '/api/v1/statistics').json()['totalRecords'] == 2


def test_multitenant_isolation_on_all_queries_and_upload(env):
    assert upload(env, packet()).status_code == 200
    other = packet()
    other['deviceId'] = 'device-b'
    assert upload(env, other, env['b']).status_code == 200
    assert upload(env, packet('forged'), env['b']).status_code == 403
    forged_tenant = packet('tenant-forged')
    forged_tenant['tenantId'] = 'unit-b'
    assert upload(env, forged_tenant).status_code == 422
    for reader, device_id in [('reader_a', 'device-a'), ('reader_b', 'device-b')]:
        assert [d['id'] for d in query(env, '/api/v1/devices', reader).json()['items']] == [device_id]
        for path in ['/api/v1/records', '/api/v1/shots']:
            data = query(env, path, reader).json()
            assert data['total'] == 1
            assert {row['device_id'] for row in data['items']} == {device_id}
        assert query(env, '/api/v1/statistics', reader).json()['totalShots'] == 1
    for path in ['/api/v1/records', '/api/v1/shots', '/api/v1/statistics']:
        assert query(env, path, params={'deviceId': 'device-b'}).status_code == 404
    record_b = query(env, '/api/v1/records', 'reader_b').json()['items'][0]['id']
    assert query(env, f'/api/v1/records/{record_b}').status_code == 404
    # Repeated, alternating requests exercise connection pool reuse.
    for i in range(10):
        reader = 'reader_a' if i % 2 else 'reader_b'
        assert query(env, '/api/v1/records', reader).json()['total'] == 1


def test_device_and_tenant_credentials_are_not_interchangeable(env):
    assert upload(env, packet(), env['reader_a']).status_code == 401
    assert env['client'].get('/api/v1/records', headers=auth(env['a'])).status_code == 401
    assert env['client'].post('/device/v1/batches', json=packet()).status_code == 401
    assert env['client'].get('/api/v1/devices', headers=auth('invalid')).status_code == 401
    with env['db'].engine.begin() as connection:
        connection.execute(devices.update().where(devices.c.id == 'device-a').values(enabled=False))
    assert upload(env, packet()).status_code == 401
    assert query(env, '/api/v1/records').status_code == 200


def test_previous_day_and_unsynced_times_are_not_upload_day(env):
    upload(env, packet())
    unsynced = packet('unsynced', sequence='2', shot=2, stamp='T+00:03:00')
    unsynced['records'][0][8:10] = [None, None]
    assert upload(env, unsynced).status_code == 200
    stats = query(env, '/api/v1/statistics').json()
    assert {row['date']: row['count'] for row in stats['shotsByDate']} == {'2026-11-15': 1, None: 1}
    data = query(env, '/api/v1/records', params={'eventDate': '2026-11-15'}).json()
    assert data['total'] == 1
    assert data['items'][0]['occurred_at'] == '2026-11-14T16:30:00+00:00'
    gps = query(env, '/api/v1/records').json()['items'][1]['payload']
    assert gps['gpsValid'] is False and gps['latitude'] is None


def test_uint64_gaps_epochs_and_old_boot_ids(env):
    p = packet(sequence=str(2**64 - 1), shot=2**32-1)
    assert upload(env, p).json()['records'][0]['sequence'] == str(2**64 - 1)
    p2 = packet('new-boot', sequence='2048', boot='1111111111111111')
    assert upload(env, p2).status_code == 200
    p3 = packet('new-epoch')
    p3['storageEpoch'] = 'epoch-2'
    assert upload(env, p3).status_code == 200
    assert query(env, '/api/v1/statistics').json()['totalShots'] == 3


@pytest.mark.parametrize('fmt', ['compact-v1', 'compact-v2', 'compact-v2-string-u64'])
def test_wire_versions_match_existing_field_order(env, fmt):
    p = packet(shot=0)
    p['format'] = fmt
    if fmt == 'compact-v1':
        p['records'][0].pop()
    if fmt != 'compact-v2-string-u64':
        p['records'][0][0] = 1
        p['records'][0][11] = 120000
    assert upload(env, p).status_code == 200
    assert query(env, '/api/v1/records').json()['items'][0]['sequence'] == '1'


@pytest.mark.parametrize('field,value', [(0, True), (0, -1), (0, str(2**64)), (0, 1.1),
    (2, 65536), (2, True), (7, -0.1), (8, 91.0), (9, None), (10, 'bad'),
    (11, -1), (12, 2**32), (13, 3), (1, '2026-02-30 00:00:00')])
def test_invalid_records_never_commit_partial_batch(env, field, value):
    p = packet()
    bad = deepcopy(p['records'][0])
    bad[0] = '2'
    bad[field] = value
    p['records'].append(bad)
    assert upload(env, p).status_code == 422
    assert query(env, '/api/v1/records').json()['total'] == 0


def test_duplicate_identity_inside_batch_and_body_limit(env):
    p = packet()
    p['records'].append(p['records'][0])
    assert upload(env, p).status_code == 422
    assert env['client'].post('/device/v1/batches', content=b'x' * 65537,
        headers=auth(env['a'])).status_code == 413
    assert query(env, '/api/v1/records').json()['total'] == 0


@pytest.mark.parametrize('version', [True, '1', 1.0, 2])
def test_protocol_version_is_strict(env, version):
    p = packet()
    p['schemaVersion'] = version
    assert upload(env, p).status_code == 422


def test_record_count_and_chunked_body_limit(env):
    p = packet(shot=0)
    p['records'] = [deepcopy(p['records'][0]) for _ in range(65)]
    assert upload(env, p).status_code == 422
    p['records'] = []
    assert upload(env, p).status_code == 422
    response = env['client'].post('/device/v1/batches',
        content=iter([b'a' * 40000, b'b' * 40000]), headers=auth(env['a']))
    assert response.status_code == 413


def test_malformed_json_and_nonfinite_numbers_are_rejected(env):
    headers = {**auth(env['a']), 'Content-Type': 'application/json'}
    assert env['client'].post('/device/v1/batches', content=b'{bad', headers=headers).status_code == 422
    import json
    p = packet()
    p['records'][0][7] = float('nan')
    assert env['client'].post('/device/v1/batches', content=json.dumps(p), headers=headers).status_code == 422
    assert query(env, '/api/v1/records').json()['total'] == 0


def test_database_write_failure_returns_no_ack_and_retry_is_safe(env):
    def fail_outbox(connection, cursor, statement, parameters, context, executemany):
        if statement.startswith('INSERT INTO outbox_jobs'):
            raise OperationalError('injected storage failure', None, Exception('disk failure'))
    event.listen(env['db'].engine, 'before_cursor_execute', fail_outbox)
    try:
        response = upload(env, packet())
        assert response.status_code == 503
        assert 'committed' not in response.json()
    finally:
        event.remove(env['db'].engine, 'before_cursor_execute', fail_outbox)
    assert query(env, '/api/v1/records').json()['total'] == 0
    assert upload(env, packet()).json()['insertedCount'] == 1


def test_commit_failure_returns_no_ack(env):
    def fail_commit(connection):
        raise OperationalError('injected commit failure', None, Exception('commit failure'))
    event.listen(env['db'].engine, 'commit', fail_commit)
    try:
        assert upload(env, packet()).status_code == 503
    finally:
        event.remove(env['db'].engine, 'commit', fail_commit)
    assert query(env, '/api/v1/records').json()['total'] == 0
    assert upload(env, packet()).json()['insertedCount'] == 1


def test_concurrent_retransmits_create_one_record_and_shot(env):
    with ThreadPoolExecutor(max_workers=4) as pool:
        results = list(pool.map(lambda i: upload(env, packet(f'retry-{i}')), range(8)))
    assert all(r.status_code == 200 for r in results)
    assert sum(r.json()['insertedCount'] for r in results) == 1
    stats = query(env, '/api/v1/statistics').json()
    assert (stats['totalRecords'], stats['totalShots']) == (1, 1)


def test_query_pagination_and_kind_filters(env):
    p = packet(shot=0)
    p['records'][0][13] = 2
    upload(env, p)
    upload(env, packet('shot', sequence='2'))
    assert query(env, '/api/v1/records', params={'kind': 'gps'}).json()['total'] == 1
    page = query(env, '/api/v1/records', params={'limit': 1, 'offset': 1}).json()
    assert page['total'] == 2 and len(page['items']) == 1
    assert query(env, '/api/v1/records', params={'limit': 100000}).status_code == 422
