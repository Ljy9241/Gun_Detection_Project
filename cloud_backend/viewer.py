"""Single-worker, read-only browser application for the existing SQLite receiver."""
import hashlib
import secrets
import sqlite3
import time
from contextlib import closing
from datetime import date, datetime, timedelta, timezone
from html import escape
import json
import os
from pathlib import Path
from urllib.parse import parse_qs, urlencode

from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse

app = FastAPI(docs_url=None, redoc_url=None, openapi_url=None)
BASE = "/viewer/"
COOKIE = "__Secure-unit-view"
PAGE_SIZE = 25
KINDS = {"sensor": "普通采样", "shot": "射击事件"}
# Hide retired test devices in the viewer without deleting their stored records.
# Set VIEWER_HIDDEN_DEVICES to an empty string to show every device again.
HIDDEN_DEVICES = tuple(value.strip() for value in
                       os.environ.get("VIEWER_HIDDEN_DEVICES", "pilot-001").split(",") if value.strip())
sessions = {}
TZ = timezone(timedelta(hours=8))
MAP_TEMPLATE = Path(__file__).with_name("viewer_map.html")
AMAP_CONFIG = Path(os.environ.get("VIEWER_AMAP_CONFIG", "/opt/gun-viewer/amap_config.json"))
# Ordinary samples can arrive before the dedicated GPS archive. Both carry a
# position from the same receiver, so the map can show an active boot too.
VALID_GPS = "kind IN ('gps','sensor') AND json_extract(payload,'$.gpsValid')=1 AND boot_id<>'0000000000000000'"
CSS = """
*{box-sizing:border-box}body{margin:0;background:#f2f5f7;color:#182f3b;font:15px/1.55 "Source Sans 3","Microsoft YaHei",sans-serif}
header{background:#123746;color:white;padding:16px max(5vw,20px);display:flex;align-items:center;justify-content:space-between}
header strong{letter-spacing:1px}main{max-width:1350px;margin:36px auto;padding:0 4vw}
h1{font-size:30px;line-height:1.25;margin:0 0 20px}button,.button{background:#12645d;color:white;padding:9px 17px;border:0;border-radius:5px;cursor:pointer;text-decoration:none;font:inherit}
header button{background:transparent;padding:4px 12px;border:1px solid #89a4ac;font-size:14px}form{margin:0}
.panel{background:white;border:1px solid #cad6da;border-radius:9px;margin-bottom:20px;padding:20px}
.filters,nav,.summary{display:flex;gap:15px;align-items:end;flex-wrap:wrap}label{display:grid;gap:4px;font-size:14px}
input,select{border:1px solid #a3b8bd;background:white;color:#183744;border-radius:5px;padding:9px;font:inherit}
.login{max-width:420px;margin:10vh auto}.login label{margin-bottom:16px}.error{color:#aa3131}
.summary,nav{justify-content:space-between;align-items:center}.summary{margin-bottom:12px}h2{font-size:19px;margin:0}
.scroll{overflow:auto}table{width:100%;border-collapse:collapse;font-size:14px;white-space:nowrap}
th,td{text-align:left;padding:10px 12px;border-bottom:1px solid #e0e8e9}th{background:#eaf1f3;color:#365461}
tbody tr:nth-child(even){background:#fafcfc}summary,a{color:#095c57;cursor:pointer}dl{display:grid;grid-template-columns:auto auto;gap:3px 12px}dd{margin:0}nav{margin-top:16px}
:focus-visible{outline:3px solid #db9d30;outline-offset:2px}
"""


def db():
    path = Path(os.environ.get("VIEWER_DB", "/var/lib/gun-receiver/receiver.db")).resolve()
    conn = sqlite3.connect(path.as_uri() + "?mode=ro", uri=True, timeout=5)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA query_only=ON")
    return conn


def h(value):
    return escape(str(value), quote=True)


def page(body, unit=None, status=200):
    account = ('<form method="post" action="/viewer/logout"><span>' + h(unit)
               + '</span> <button>退出</button></form>') if unit else ""
    doc = ('<!doctype html><html lang="zh-CN"><meta charset="utf-8">'
           '<meta name="viewport" content="width=device-width,initial-scale=1">'
           '<title>检测数据查询</title><style>' + CSS + '</style><header><strong>检测数据查询</strong>'
           + account + '</header><main>' + body + '</main></html>')
    return HTMLResponse(doc, status_code=status)


@app.middleware("http")
async def privacy(request, next_handler):
    response = await next_handler(request)
    response.headers.update({"Cache-Control": "no-store", "Referrer-Policy": "no-referrer",
        "X-Content-Type-Options": "nosniff"})
    response.headers.setdefault("Content-Security-Policy",
        "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'")
    return response


def login_form(error=""):
    return page('<section class="panel login"><h1>用户登录</h1><form method="post" action="/viewer/login">'
                '<label>用户账号<input name="unit" autocomplete="username" required></label>'
                '<label>查询密码<input name="password" type="password" autocomplete="current-password" required></label>'
                + ('<p class="error">' + h(error) + '</p>' if error else '')
                + '<button>登录并查看记录</button></form></section>', status=401 if error else 200)


def identity(conn, digest):
    return conn.execute("""SELECT t.id,t.name FROM tenants t JOIN tenant_readers r ON t.id=r.tenant_id
                           WHERE r.token_hash=? AND r.enabled=1""", (digest,)).fetchone()


@app.post(BASE + "login")
async def login(request: Request):
    raw = await request.body()
    if len(raw) > 2048:
        return login_form("登录信息过长")
    try:
        fields = parse_qs(raw.decode(), max_num_fields=2)
        digest = hashlib.sha256(fields["password"][0].encode()).hexdigest()
        with closing(db()) as conn:
            unit = identity(conn, digest)
        if not unit or unit["id"] != fields["unit"][0]:
            return login_form("用户账号或查询密码有误")
    except (KeyError, ValueError):
        return login_form("请输入用户账号和查询密码")
    now = time.monotonic()
    for sid in list(sessions):
        if sessions[sid][1] <= now:
            sessions.pop(sid)
    sessions.pop(request.cookies.get(COOKIE), None)
    sid = secrets.token_urlsafe(32)
    sessions[sid] = (digest, now + 8 * 3600)
    result = RedirectResponse(BASE, status_code=303)
    result.set_cookie(COOKIE, sid, secure=True, httponly=True, samesite="strict", path=BASE, max_age=8*3600)
    return result


@app.post(BASE + "logout")
def logout(request: Request):
    sessions.pop(request.cookies.get(COOKIE), None)
    result = RedirectResponse(BASE, status_code=303)
    result.delete_cookie(COOKIE, path=BASE, secure=True, httponly=True, samesite="strict")
    return result


def option(value, label, current):
    return f'<option value="{h(value)}"{" selected" if current == value else ""}>{h(label)}</option>'


def authorized_unit(request):
    digest, expiry = sessions.get(request.cookies.get(COOKIE), (None, 0))
    if expiry <= time.monotonic():
        return None
    with closing(db()) as conn:
        return identity(conn, digest)


def map_sessions(conn, tenant_id):
    return conn.execute("""SELECT device_id,storage_epoch,boot_id,COUNT(*) AS count,
        MIN(event_date) AS day,MAX(received_at) AS last_received
        FROM records WHERE tenant_id=? AND device_id NOT IN (SELECT value FROM json_each(?)) AND """ + VALID_GPS + """
        GROUP BY device_id,storage_epoch,boot_id HAVING COUNT(*)>=3
        ORDER BY last_received DESC""", (tenant_id, json.dumps(HIDDEN_DEVICES))).fetchall()


@app.get(BASE + "map")
def map_page(request: Request):
    unit = authorized_unit(request)
    if not unit:
        return login_form()
    try:
        settings = json.loads(AMAP_CONFIG.read_text(encoding="utf-8-sig"))
        if not all(isinstance(settings.get(key), str) and settings[key] for key in ("key", "securityJsCode")):
            raise ValueError("incomplete AMap configuration")
        template = MAP_TEMPLATE.read_text(encoding="utf-8")
    except (OSError, ValueError):
        return page('<h1>GPS 轨迹地图</h1><p>地图配置暂不可用。</p><a href="/viewer/">返回记录</a>',unit["name"],503)
    settings = json.dumps({key: settings[key] for key in ("key", "securityJsCode")}).replace("<", "\\u003c")
    response = HTMLResponse(template.replace("__AMAP_CONFIG__", settings))
    response.headers["Content-Security-Policy"] = (
        "default-src 'none'; script-src 'unsafe-inline' 'unsafe-eval' https://webapi.amap.com "
        "https://*.amap.com https://*.autonavi.com; connect-src 'self' https://*.amap.com "
        "https://*.autonavi.com; worker-src blob:; img-src data: blob: https:; style-src 'unsafe-inline' https:; "
        "font-src data: https:; form-action 'none'; frame-ancestors 'none'; base-uri 'none'")
    return response


@app.get(BASE + "map/sessions")
def map_session_list(request: Request):
    unit = authorized_unit(request)
    if not unit:
        return JSONResponse({"error": "请重新登录"}, status_code=401)
    with closing(db()) as conn:
        rows = map_sessions(conn, unit["id"])
    items = [dict(device=row["device_id"], epoch=row["storage_epoch"], boot=row["boot_id"],
                  count=row["count"], label=f'{row["device_id"]} · {row["day"] or "未校时"} · {row["count"]} 点 · {row["boot_id"][-8:]}')
             for row in rows]
    return JSONResponse({"items": items})


@app.get(BASE + "map/track")
def map_track(request: Request, device: str, epoch: str, boot: str):
    unit = authorized_unit(request)
    if not unit:
        return JSONResponse({"error": "请重新登录"}, status_code=401)
    if device in HIDDEN_DEVICES:
        return JSONResponse({"error": "设备不可见"}, status_code=404)
    with closing(db()) as conn:
        conn.execute("BEGIN")
        eligible = conn.execute("""SELECT COUNT(*) FROM records WHERE tenant_id=? AND
            device_id=? AND storage_epoch=? AND boot_id=? AND """ + VALID_GPS,
            (unit["id"], device, epoch, boot)).fetchone()[0]
        if eligible < 3:
            return JSONResponse({"error": "这次上电的有效定位点不足 3 个"}, status_code=404)
        step = max(1, (eligible + 9999) // 10000)
        gps = conn.execute("""WITH ordered AS (SELECT payload,uptime_ms,
            ROW_NUMBER() OVER (ORDER BY CAST(uptime_ms AS INTEGER),received_at,id) AS n
            FROM records WHERE tenant_id=? AND device_id=? AND storage_epoch=? AND boot_id=?
            AND """ + VALID_GPS + """ ) SELECT payload,uptime_ms FROM ordered
            WHERE n=1 OR n=? OR n%?=0 ORDER BY n""",
            (unit["id"], device, epoch, boot, eligible, step)).fetchall()
        shots = conn.execute("""SELECT payload,uptime_ms FROM records WHERE tenant_id=?
            AND device_id=? AND storage_epoch=? AND boot_id=? AND kind='shot'
            ORDER BY CAST(uptime_ms AS INTEGER),received_at,id LIMIT 5000""",
            (unit["id"], device, epoch, boot)).fetchall()
    def point(row):
        payload = json.loads(row["payload"])
        valid = bool(payload.get("gpsValid"))
        return dict(ms=int(row["uptime_ms"]), time=payload.get("timestamp") or "未校时",
                    lat=payload.get("latitude") if valid else None,
                    lon=payload.get("longitude") if valid else None, valid=valid)
    return JSONResponse(dict(device=device, epoch=epoch, boot=boot, raw_count=eligible,
                             bucket_ms=step*1000, points=[point(row) for row in gps],
                             shots=[dict(point(row), shot=json.loads(row["payload"]).get("shotId")) for row in shots]))


def render_row(row, show_event_number=False):
    payload = json.loads(row["payload"])
    received = datetime.fromisoformat(row["received_at"]).astimezone(TZ).strftime("%Y-%m-%d %H:%M:%S")
    coordinates = "—"
    if payload.get("gpsValid") and payload.get("latitude") is not None and payload.get("longitude") is not None:
        coordinates = f'{payload["latitude"]:.7f}, {payload["longitude"]:.7f}'
    readings = ("mic_raw", "mic_p2p", "mic_rms")
    kind_label = KINDS.get(row["kind"], row["kind"])
    if row["kind"] == "shot" and not show_event_number and payload.get("shotId"):
        kind_label += f'（编号 {payload["shotId"]}）'
    stamp = payload.get("timestamp", "—")
    if payload.get("timestamp_source") == "inferred":
        stamp += "（推算）"
    values = [row["device_id"], kind_label, received, stamp]
    if row["kind"] == "sensor" and payload.get("tilt_valid"):
        values.extend([f'{payload["roll_deg"]:.2f}°',
                       f'{payload["pitch_deg"]:.2f}°'])
    else:
        values.extend(["—", "—"])
    values.extend(payload.get(field, "—") if row["kind"] == "sensor" else "—" for field in readings)
    values.append(coordinates)
    if show_event_number:
        values.append(payload.get("shotId") or "—")
    return "<tr>" + "".join("<td>" + h(value) + "</td>" for value in values) + "</tr>"


@app.get(BASE)
def view(request: Request, device: str = "", day: str = "", kind: str = "", p: int = 1, snapshot: str = ""):
    digest, expiry = sessions.get(request.cookies.get(COOKIE), (None, 0))
    if expiry <= time.monotonic():
        return login_form()
    with closing(db()) as conn:
        conn.execute("BEGIN")
        unit = identity(conn, digest)
        if not unit:
            return login_form("查询权限已失效")
        if kind == "gps":
            return RedirectResponse(BASE + "map", status_code=303)
        devices = conn.execute("SELECT id,name FROM devices WHERE tenant_id=? AND id NOT IN (SELECT value FROM json_each(?)) ORDER BY id",(unit["id"], json.dumps(HIDDEN_DEVICES))).fetchall()
        try:
            if day: day = date.fromisoformat(day).isoformat()
            if kind and kind not in KINDS: raise ValueError()
            if snapshot:
                stamp = datetime.fromisoformat(snapshot)
                if stamp.tzinfo is None: raise ValueError()
            else:
                stamp = datetime.now(timezone.utc)
        except ValueError:
            return page('<p>筛选条件无效。</p><a href="/viewer/">返回记录</a>', unit["name"], 400)
        snapshot = min(stamp.astimezone(timezone.utc), datetime.now(timezone.utc)).isoformat()
        clauses = ["tenant_id=?", "received_at<=?", "kind IN ('sensor','shot')",
                   "device_id NOT IN (SELECT value FROM json_each(?))"]
        args = [unit["id"], snapshot, json.dumps(HIDDEN_DEVICES)]
        for column, value in (("device_id",device),("event_date",day),("kind",kind)):
            if value:
                clauses.append(column+"=?")
                args.append(value)
        where = " AND ".join(clauses)
        total = conn.execute("SELECT count(*) FROM records WHERE "+where,args).fetchone()[0]
        pages = max(1,(total+PAGE_SIZE-1)//PAGE_SIZE)
        p = max(1,min(p,pages))
        rows = conn.execute("SELECT device_id,kind,received_at,payload FROM records WHERE "+where+
            " ORDER BY (event_date IS NULL),"
            " CASE WHEN event_date IS NOT NULL THEN json_extract(payload,'$.timestamp') END DESC,"
            " device_id,storage_epoch,LENGTH(sequence) DESC,sequence DESC,id DESC"
            " LIMIT ? OFFSET ?",args+[PAGE_SIZE,(p-1)*PAGE_SIZE]).fetchall()
    devopts = option("", "全部设备", device) + "".join(option(d["id"], d["name"]+" · "+d["id"], device) for d in devices)
    kinds = option("", "全部类型", kind) + "".join(option(k,v,kind) for k,v in KINDS.items())
    show_event_number = kind == "shot"
    event_header = '<th>事件编号</th>' if show_event_number else ''
    column_count = 11 if show_event_number else 10
    def link(number,label):
        params=urlencode(dict(device=device,day=day,kind=kind,p=number,snapshot=snapshot))
        return '<a href="?'+h(params)+'">'+label+'</a>'
    body = ('<h1>设备记录</h1><form class="panel filters" method="get" action="/viewer/">'
            '<label>设备<select name="device">'+devopts+'</select></label>'
            '<label>采集日期<input type="date" name="day" value="'+h(day)+'"></label>'
            '<label>记录类型<select name="kind">'+kinds+'</select></label><button>查询</button>'
            '<a href="/viewer/">查看全部</a><a href="/viewer/map">GPS 轨迹地图</a></form><section class="panel"><div class="summary">'
            '<h2>查询结果</h2><span>共 '+str(total)+' 条 · 所有设备按生成时间倒序（含推算）；无时间基准的记录置后，按设备及生成序号倒序</span></div><div class="scroll"><table>'
            '<thead><tr><th>设备</th><th>类型</th><th>接收时间（北京）</th><th>采集时间</th><th>横滚角 Roll (°)</th><th>俯仰角 Pitch (°)</th><th>麦克风原值</th><th>峰峰值</th><th>均方根</th><th>经纬度</th>'+event_header+'</tr></thead>'
            '<tbody>'+(''.join(render_row(r, show_event_number) for r in rows) or '<tr><td colspan="'+str(column_count)+'">当前条件下没有记录</td></tr>')+
            '</tbody></table></div><nav><span>'+ (link(p-1,"上一页") if p>1 else "")+'</span><span>第 '
            + str(p)+' / '+str(pages)+' 页</span><span>'+(link(p+1,"下一页") if p<pages else "")+'</span></nav></section>')
    return page(body, unit["name"])
