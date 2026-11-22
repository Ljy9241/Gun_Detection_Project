import json

import pytest

from cloud_backend.protocol import Batch, normalize
from cloud_backend.viewer import render_row
from cloud_backend.tests.test_receiver import env, packet, upload


def tilt_packet(roll=-123.45, pitch=67.89, valid=True):
    data = packet(shot=0)
    data['format'] = 'compact-v3-tilt'
    data['records'][0][2:5] = [roll, pitch, valid]
    data['records'][0].append(1)
    return data


def test_tilt_upload_persistence_replay_and_display(env):
    data = tilt_packet()
    assert upload(env, data).json()['insertedCount'] == 1
    assert upload(env, data).json()['duplicateCount'] == 1
    payload = normalize(Batch(**data))[0]['payload']
    assert payload['roll_deg'] == -123.45
    assert payload['pitch_deg'] == 67.89
    assert payload['tilt_valid'] is True
    assert 'accel_x' not in payload
    row = dict(payload=json.dumps(payload), received_at='2026-11-22T00:00:00+00:00',
               device_id='device-a', kind='sensor')
    html = render_row(row)
    assert '-123.45°' in html and '67.89°' in html
    payload.update(tilt_valid=False, roll_deg=None, pitch_deg=None)
    row['payload'] = json.dumps(payload)
    html = render_row(row)
    assert '未校准/无效' not in html
    assert html.count('<td>') == 10
    assert '<td>—</td><td>—</td>' in html


def test_invalid_calibration_is_null():
    payload = normalize(Batch(**tilt_packet(None, None, False)))[0]['payload']
    assert payload['roll_deg'] is None and payload['tilt_valid'] is False


@pytest.mark.parametrize('angles', [(181, 0, True), (0, -91, True),
    (None, 0, True), (0, 0, False), (0, 0, 1), (float('nan'), 0, True)])
def test_bad_angles_rejected(angles):
    with pytest.raises(ValueError):
        normalize(Batch(**tilt_packet(*angles)))


def test_old_flash_retry_preserves_hash_and_can_mix_with_tilt(env):
    old = packet(shot=0)
    assert upload(env, old).json()['insertedCount'] == 1
    upgraded = packet('upgraded', shot=0)
    upgraded['format'] = 'compact-v3-tilt'
    upgraded['records'][0].append(0)
    assert normalize(Batch(**old)) == normalize(Batch(**upgraded))
    tilt = tilt_packet()['records'][0]
    tilt[0] = '2'
    upgraded['records'].append(tilt)
    ack = upload(env, upgraded).json()
    assert ack['insertedCount'] == 1 and ack['duplicateCount'] == 1
