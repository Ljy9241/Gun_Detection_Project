"""Real HTTP checks using a temporary database; never seeds the deployed database."""
import argparse
from copy import deepcopy
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


def verify(source):
    source = Path(source).resolve(strict=True)
    sys.path.insert(0, str(source))
    from cloud_backend.database import Database
    from cloud_backend.service import provision_device, provision_tenant

    checks = []
    with tempfile.TemporaryDirectory(prefix='gun-receiver-validation-') as directory:
        url = f'sqlite:///{(Path(directory) / "validation.db").as_posix()}'
        db = Database(url)
        db.initialize()
        read_a = provision_tenant(db, 'validation-a', 'Temporary validation A')
        read_b = provision_tenant(db, 'validation-b', 'Temporary validation B')
        write_a = provision_device(db, 'validation-a', 'validation-device-a', 'Temporary A')
        write_b = provision_device(db, 'validation-b', 'validation-device-b', 'Temporary B')
        db.engine.dispose()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        base = f'http://127.0.0.1:{port}'
        environment = dict(os.environ, GUN_DATABASE_URL=url)
        process = None

        def call(path, token=None, payload=None):
            headers = {'Content-Type': 'application/json'}
            if token:
                headers['Authorization'] = f'Bearer {token}'
            body = None if payload is None else json.dumps(payload).encode('utf-8')
            request = Request(base + path, data=body, headers=headers)
            try:
                with urlopen(request, timeout=10) as response:
                    return response.status, json.load(response)
            except HTTPError as error:
                return error.code, json.load(error)

        with (Path(directory) / 'server.log').open('w+b') as server_log:
            def start():
                nonlocal process
                process = subprocess.Popen([sys.executable, '-m', 'uvicorn',
                    'cloud_backend.main:app', '--host', '127.0.0.1', '--port', str(port)],
                    cwd=source, env=environment, stdout=server_log, stderr=subprocess.STDOUT,
                    creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                for _ in range(100):
                    if process.poll() is not None:
                        raise RuntimeError('Temporary receiver exited during startup')
                    try:
                        if call('/healthz')[0] == 200:
                            return
                    except (URLError, ConnectionError, TimeoutError):
                        pass
                    time.sleep(0.1)
                raise RuntimeError('Temporary receiver startup timed out')

            def stop():
                if process is not None and process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)

            try:
                start()
                checks.append('real_http_health')
                assert call('/api/v1/records')[0] == 401
                checks.append('unauthenticated_query_rejected')
                batch = dict(schemaVersion=1, deviceId='validation-device-a',
                    storageEpoch='validation-epoch', batchId='validation-batch',
                    format='compact-v2-string-u64', records=[
                        ['9007199254740993', '2026-11-15 00:30:00', 1, 2, 3, 4, 5,
                         0.1, None, None, '0123456789abcdef', '120000', 1, 0]])
                code, first = call('/device/v1/batches', write_a, batch)
                assert code == 200 and first['committed'] and first['insertedCount'] == 1
                assert first['records'][0]['sequence'] == '9007199254740993'
                checks.append('upload_committed_and_uint64_preserved')
                code, replay = call('/device/v1/batches', write_a, batch)
                assert code == 200 and replay['duplicateCount'] == 1 and replay['insertedCount'] == 0
                checks.append('response_loss_replay_deduplicated')
                changed = deepcopy(batch)
                changed['records'][0][2] = 99
                assert call('/device/v1/batches', write_a, changed)[0] == 409
                checks.append('conflicting_content_rejected')
                assert call('/device/v1/batches', write_b, batch)[0] == 403
                checks.append('cross_device_write_rejected')
                b_batch = deepcopy(batch)
                b_batch['deviceId'] = 'validation-device-b'
                assert call('/device/v1/batches', write_b, b_batch)[0] == 200
                for token, device_id in [(read_a, 'validation-device-a'), (read_b, 'validation-device-b')]:
                    for path in ['/api/v1/records', '/api/v1/shots']:
                        code, result = call(path, token)
                        assert code == 200 and result['total'] == 1
                        assert {row['device_id'] for row in result['items']} == {device_id}
                checks.append('tenant_records_and_shots_isolated')
                b_record = call('/api/v1/records', read_b)[1]['items'][0]['id']
                assert call('/api/v1/records/' + b_record, read_a)[0] == 404
                checks.append('cross_tenant_record_detail_rejected')
                stats = call('/api/v1/statistics', read_a)[1]
                assert stats['totalShots'] == 1
                assert stats['shotsByDate'] == [{'date': '2026-11-15', 'count': 1}]
                checks.append('late_event_uses_original_beijing_date')
                stop()
                start()
                batch['batchId'] = 'after-service-restart'
                code, retry = call('/device/v1/batches', write_a, batch)
                assert code == 200 and retry['duplicateCount'] == 1
                assert call('/api/v1/statistics', read_a)[1]['totalShots'] == 1
                checks.append('restart_and_rebatched_retry_deduplicated')
            finally:
                stop()
    return dict(passed=len(checks), checks=checks, database='temporary_only', source=str(source))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', default=str(Path(__file__).resolve().parents[1]))
    arguments = parser.parse_args()
    print(json.dumps(verify(arguments.source), indent=2))
