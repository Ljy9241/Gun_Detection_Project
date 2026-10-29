"""Embedded AMap page; the key stays in local configuration, never in source."""
import json
import sqlite3
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
from pathlib import Path

from PyQt5.QtCore import QObject, QUrl, pyqtSignal, pyqtSlot
from PyQt5.QtWidgets import QWidget, QVBoxLayout, QLabel, QPushButton

import trajectory_store

try:
    from PyQt5.QtWebChannel import QWebChannel
    from PyQt5.QtWebEngineWidgets import QWebEngineView, QWebEngineSettings
except ImportError:
    QWebEngineView = None


class MapBridge(QObject):
    trackReady = pyqtSignal(str, int)

    def __init__(self, owner):
        super().__init__(owner)
        self.owner = owner
        self.worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix='map-history')
        self.destroyed.connect(lambda: self.worker.shutdown(wait=False))

    @pyqtSlot(result=str)
    def sessions(self):
        return self.owner.read_sessions()

    @pyqtSlot(str, str, int, str)
    def requestTrack(self, device, boot, generation, known_revision):
        future = self.worker.submit(self.owner.read_track, device, boot, known_revision)
        def finished(result):
            try:
                self.trackReady.emit(result.result(), generation)
            except RuntimeError:
                pass  # Window closed while its read-only query was finishing.
        future.add_done_callback(finished)

    @pyqtSlot(result=str)
    def current(self):
        return json.dumps(self.owner.current_session)


class TrajectoryMap(QWidget):
    def __init__(self, database_path):
        super().__init__()
        self.database_path = database_path
        self.current_session = None
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        if QWebEngineView is None:
            label = QLabel("地图需要 PyQtWebEngine。请在运行此程序的 Python 环境中安装：\n"
                           "python -m pip install PyQtWebEngine\n安装后重新启动界面；轨迹仍会保存。")
            label.setWordWrap(True)
            layout.addWidget(label)
            return
        self.view = QWebEngineView(self)
        self.view.settings().setAttribute(QWebEngineSettings.LocalContentCanAccessRemoteUrls, True)
        self.channel = QWebChannel(self.view.page())
        self.bridge = MapBridge(self)
        self.channel.registerObject('history', self.bridge)
        self.view.page().setWebChannel(self.channel)
        self.error_label = QLabel()
        self.error_label.setWordWrap(True)
        layout.addWidget(self.error_label)
        retry = QPushButton('重新加载地图')
        retry.clicked.connect(self.load)
        layout.addWidget(retry)
        layout.addWidget(self.view, 1)
        self.load()

    def load(self):
        folder = Path(__file__).resolve().parent
        try:
            config = json.loads((folder / 'amap_config.json').read_text(encoding='utf-8-sig'))
            if not isinstance(config.get('key'), str) or not config['key'].strip():
                raise ValueError('缺少 key')
            if not isinstance(config.get('securityJsCode'), str) or not config['securityJsCode'].strip():
                raise ValueError('缺少 securityJsCode')
            template = (folder / 'trajectory_map.html').read_text(encoding='utf-8')
            settings = json.dumps({k: config[k] for k in ('key', 'securityJsCode')}).replace('<', '\\u003c')
            self.view.setHtml(template.replace('__AMAP_CONFIG__', settings), QUrl.fromLocalFile(str(folder / 'trajectory_map.html')))
            self.error_label.hide()
        except (OSError, ValueError, TypeError):
            self.error_label.setText('地图配置读取失败。请检查 data_monitor/amap_config.json 的 key 和 securityJsCode。')
            self.error_label.show()

    def set_current_session(self, packet):
        self.current_session = dict(device=str(packet.get('deviceId', '未知设备')),
                                    boot=trajectory_store.boot_id(packet))

    def _read(self, callback):
        if not self.database_path.exists():
            return json.dumps({'error': '尚无轨迹，请先开始监听。'}, ensure_ascii=False)
        try:
            # Read-only connections cannot accidentally create or migrate a database.
            connection = sqlite3.connect(self.database_path.as_uri() + '?mode=ro', uri=True, timeout=0.2)
            try:
                return json.dumps(callback(connection), ensure_ascii=False, allow_nan=False)
            finally:
                connection.close()
        except (sqlite3.Error, ValueError):
            return json.dumps({'error': '轨迹暂不可读，稍后自动重试。'}, ensure_ascii=False)

    def read_sessions(self):
        def read(db):
            items = trajectory_store.sessions(db)
            for item in items:
                time_label = item['timestamp'] or f"上电计时 T+{item['start_ms'] / 1000:.0f} 秒"
                item['label'] = ('旧版未分组' if item['boot'] == 'legacy' else f"{time_label} · {item['boot'][-8:]}")
            return items
        return self._read(read)

    def read_track(self, device, boot, known_revision=''):
        def read(db):
            # One snapshot prevents uploads changing the revision midway through a read.
            db.execute('BEGIN')
            row = db.execute('SELECT revision FROM track_sessions WHERE device=? AND boot=?',
                             (device, boot)).fetchone()
            known = json.loads(known_revision) if known_revision else []
            if row and len(known) == 3 and known[2] == row[0]:
                return dict(unchanged=True)
            result = trajectory_store.track(db, device, boot)
            return result
        return self._read(read)
