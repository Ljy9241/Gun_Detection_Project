import csv
import io
import re
from datetime import datetime, timedelta
from html import unescape

import pytest
from fastapi.testclient import TestClient

from cloud_backend import viewer
from cloud_backend.tests.test_receiver import env, packet, upload


@pytest.fixture
def browsing(env, monkeypatch):
    base = datetime(2026, 11, 15, 0, 30)
    for offset in range(0, 205, 64):
        data = packet('browse-'+str(offset), shot=0)
        data['records'] = [packet(sequence=str(i+1), shot=0,
            stamp=(base+timedelta(seconds=i)).strftime('%Y-%m-%d %H:%M:%S'))['records'][0]
            for i in range(offset, min(offset+64, 205))]
        assert upload(env, data).status_code == 200
    assert upload(env, packet('unknown', sequence='206', shot=0, stamp='T+00:00:01')).status_code == 200
    other = packet('other-tenant', shot=0)
    other['deviceId'] = 'device-b'
    assert upload(env, other, env['b']).status_code == 200
    monkeypatch.setenv('VIEWER_DB', str(env['db'].engine.url.database))
    viewer.sessions.clear()
    with TestClient(viewer.app, base_url='https://example.test') as client:
        assert client.post('/viewer/login', data={'unit': 'unit-a', 'password': env['reader_a']},
                           follow_redirects=False).status_code == 303
        yield client


def row_count(html):
    return len(re.findall(r'<tr><td>device-a</td>', html))


def test_sizes_jump_and_filter_retention(browsing):
    assert row_count(browsing.get('/viewer/').text) == 100
    for size in (25, 50, 100, 200):
        result = browsing.get('/viewer/', params={'page_size': size, 'device': 'device-a', 'kind': 'sensor'})
        assert result.status_code == 200 and row_count(result.text) == size
        next_url = unescape(re.search(r'<a href="([^"]+)">下一页</a>', result.text)[1])
        assert f'page_size={size}' in next_url and 'device=device-a' in next_url and 'snapshot=' in next_url
        assert row_count(browsing.get('/viewer/'+next_url).text) == min(size, 206-size)
    assert row_count(browsing.get('/viewer/?page_size=100&p=3').text) == 6
    assert '第 3 / 3 页' in browsing.get('/viewer/?p=999').text
    for size in (0, 201, -1):
        assert browsing.get('/viewer/', params={'page_size': size}).status_code == 400


def test_range_export_and_refresh(browsing):
    params = dict(start='2026-11-15T00:30:00', end='2026-11-15T00:30:02', page_size=25)
    html = browsing.get('/viewer/', params=params).text
    assert row_count(html) == 3
    assert 'T+00:00:01' not in html
    result = browsing.get('/viewer/export.csv', params=params)
    assert result.content.startswith(b'\xef\xbb\xbf')
    rows = list(csv.reader(io.StringIO(result.content.decode('utf-8-sig'))))
    assert len(rows) == 4 and rows[1][3] == '2026-11-15 00:30:02'
    all_rows = list(csv.reader(io.StringIO(browsing.get('/viewer/export.csv?page_size=25').content.decode('utf-8-sig'))))
    assert len(all_rows) == 207  # All pages, not just 25 or 100 rows.
    assert {r[0] for r in all_rows[1:]} == {'device-a'}
    assert all_rows[-1][4] == '未校时'
    first = browsing.get('/viewer/?auto_refresh=1')
    assert 'data-active="1"' in first.text and "script-src 'nonce-" in first.headers['content-security-policy']
    refresh_url = unescape(re.search(r'data-url="([^"]+)"', first.text)[1])
    assert 'snapshot=' not in refresh_url and 'auto_refresh=1' in refresh_url
    assert 'data-active="0"' in browsing.get('/viewer/?p=2&auto_refresh=1').text
    assert 'data-active="0"' in browsing.get('/viewer/').text
    for path in ('/viewer/', '/viewer/export.csv'):
        assert browsing.get(path, params={'start': params['end'], 'end': params['start']}).status_code == 400
        assert browsing.get(path, params={'start': 'invalid'}).status_code == 400
    empty = browsing.get('/viewer/export.csv?device=not-mine')
    assert len(list(csv.reader(io.StringIO(empty.content.decode('utf-8-sig'))))) == 1
    browsing.post('/viewer/logout')
    assert 'Content-Disposition' not in browsing.get('/viewer/export.csv').headers


def test_csv_formula_cells():
    assert viewer.csv_cell('=HYPERLINK("bad")').startswith("'")
    assert viewer.csv_cell('\t+cmd').startswith("'")
    assert viewer.csv_cell(-12.3) == -12.3
