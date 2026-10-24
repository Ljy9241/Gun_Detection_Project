"""ESP32-S3 检测设备的 UDP/JSON 上位机：实时显示遥测数据并支持 CSV 导出。"""

import csv
import json
import sqlite3
import socket
import sys
import threading
import time
from collections import deque
from datetime import datetime
from pathlib import Path

import pyqtgraph as pg
from PyQt5.QtCore import QEvent, QObject, QTimer, Qt, pyqtSignal
from PyQt5.QtWidgets import (
    QApplication,
    QCheckBox,
    QFileDialog,
    QFormLayout,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QMessageBox,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)


# 曲线显示最近一段时间的数据；定时器只负责刷新界面，不参与网络接收。
HISTORY_SECONDS = 60
UI_REFRESH_MS = 100
CSV_FIELDS = [
    "received_at", "uptime_ms", "device_id", "rssi", "shot_count",
    "voice_detection_count", "vibration_detection_count",
    "mic_raw", "mic_p2p", "mic_rms", "vibration_x", "vibration_y",
    "vibration_z", "vibration_magnitude", "vibration_jerk", "roll_deg",
    "pitch_deg", "gps_valid",
    "latitude", "longitude", "altitude_m", "speed_kmph", "satellites",
]
DATABASE_PATH = Path(__file__).resolve().with_name("gun_telemetry.db")


class UdpReceiver(QObject):
    """在后台线程接收 UDP 数据，并通过 Qt 信号安全地交给界面线程。"""
    packet_received = pyqtSignal(dict)
    receiver_error = pyqtSignal(str)
    listening = pyqtSignal(bool, str)

    def __init__(self):
        super().__init__()
        self._stop = threading.Event()
        self._thread = None
        self._socket = None

    def start(self, host, port):
        """停止旧监听后，以指定地址和端口启动新的后台接收线程。"""
        self.stop()
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._receive_loop, args=(host, port), daemon=True
        )
        self._thread.start()

    def stop(self):
        """设置停止标志并关闭套接字，以唤醒阻塞中的 recvfrom。"""
        self._stop.set()
        if self._socket:
            try:
                self._socket.close()
            except OSError:
                pass
            self._socket = None

    def _receive_loop(self, host, port):
        """绑定 UDP 端口、解析 JSON 包，并报告非法数据或套接字错误。"""
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket = sock
        database = None
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((host, port))
            sock.settimeout(0.5)
            database = sqlite3.connect(DATABASE_PATH, timeout=10)
            database.execute("PRAGMA journal_mode=WAL")
            database.execute("PRAGMA synchronous=FULL")
            database.executescript(
                "CREATE TABLE IF NOT EXISTS telemetry ("
                "timestamp TEXT, accel_x INTEGER, accel_y INTEGER, accel_z INTEGER, "
                "mic_raw INTEGER, mic_p2p INTEGER, mic_rms REAL, latitude REAL, longitude REAL);"
                "CREATE TABLE IF NOT EXISTS upload_receipts ("
                "device_id TEXT NOT NULL, sequence INTEGER NOT NULL, "
                "PRIMARY KEY (device_id, sequence));"
            )
            database.commit()
            self.listening.emit(True, f"监听中：{host}:{port}")
            while not self._stop.is_set():
                try:
                    data, address = sock.recvfrom(65535)
                except socket.timeout:
                    continue
                except OSError:
                    if not self._stop.is_set():
                        raise
                    break
                try:
                    packet = json.loads(data.decode("utf-8-sig").strip())
                    if isinstance(packet, dict):
                        packet["_sender"] = f"{address[0]}:{address[1]}"
                        if packet.get("type") == "telemetry":
                            try:
                                self._store_telemetry_and_ack(database, sock, packet, address)
                            except (sqlite3.DatabaseError, KeyError, TypeError, ValueError) as exc:
                                self.receiver_error.emit(f"遥测入库失败，未确认设备记录：{exc}")
                        elif packet.get("type") == "telemetry_batch":
                            try:
                                self._store_telemetry_batch_and_ack(database, sock, packet, address)
                            except (sqlite3.DatabaseError, KeyError, TypeError, ValueError) as exc:
                                self.receiver_error.emit(f"遥测批次入库失败，未确认设备记录：{exc}")
                        # 批量遥测只用于数据库入库；状态包继续驱动实时界面，避免批次包覆盖状态摘要。
                        if packet.get("type") != "telemetry_batch":
                            self.packet_received.emit(packet)
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    self.receiver_error.emit(f"收到非 JSON 数据：{exc}")
                except OSError as exc:
                    self.receiver_error.emit(f"遥测 ACK 发送失败，设备会重传：{exc}")
            self.listening.emit(False, "已停止监听")
        except (OSError, sqlite3.DatabaseError) as exc:
            self.listening.emit(False, "监听失败")
            self.receiver_error.emit(f"无法监听 {host}:{port}：{exc}")
        finally:
            if database is not None:
                database.close()
            try:
                sock.close()
            except OSError:
                pass
            if self._socket is sock:
                self._socket = None

    @staticmethod
    def _store_telemetry_and_ack(database, sock, packet, address):
        """提交记录及去重凭据后再 ACK，避免设备过早删除未落盘数据。"""
        device_id = str(packet["deviceId"])
        sequence = int(packet["sequence"])
        timestamp = packet.get("timestamp") if packet.get("timestampValid") else None
        with database:
            cursor = database.execute(
                "INSERT OR IGNORE INTO upload_receipts(device_id, sequence) VALUES (?, ?)",
                (device_id, sequence),
            )
            if cursor.rowcount:
                database.execute(
                    "INSERT INTO telemetry(timestamp, accel_x, accel_y, accel_z, "
                    "mic_raw, mic_p2p, mic_rms, latitude, longitude) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (
                        timestamp, packet.get("accel_x"), packet.get("accel_y"),
                        packet.get("accel_z"), packet.get("mic_raw"), packet.get("mic_p2p"),
                        packet.get("mic_rms"), packet.get("latitude"), packet.get("longitude"),
                    ),
                )
        acknowledgement = json.dumps(
            {"type": "ack", "deviceId": device_id, "sequence": sequence},
            separators=(",", ":"),
        ).encode("utf-8")
        sock.sendto(acknowledgement, address)

    @staticmethod
    def _store_telemetry_batch_and_ack(database, sock, packet, address):
        """一个事务写入整批记录；事务提交成功后才一次确认整批序号。"""
        device_id = str(packet["deviceId"])
        records = packet["records"]
        if not isinstance(records, list) or not records or len(records) > 64:
            raise ValueError("批次记录数无效")

        sequences = []
        new_rows = []
        seen_sequences = set()
        with database:
            for record in records:
                if not isinstance(record, dict):
                    raise TypeError("批次记录格式无效")
                sequence = int(record["sequence"])
                if sequence in seen_sequences:
                    raise ValueError("批次中存在重复序号")
                seen_sequences.add(sequence)
                sequences.append(sequence)
                cursor = database.execute(
                    "INSERT OR IGNORE INTO upload_receipts(device_id, sequence) VALUES (?, ?)",
                    (device_id, sequence),
                )
                if cursor.rowcount:
                    timestamp = record.get("timestamp") if record.get("timestampValid") else None
                    new_rows.append((
                        timestamp, record.get("accel_x"), record.get("accel_y"),
                        record.get("accel_z"), record.get("mic_raw"), record.get("mic_p2p"),
                        record.get("mic_rms"), record.get("latitude"), record.get("longitude"),
                    ))

            if new_rows:
                database.executemany(
                    "INSERT INTO telemetry(timestamp, accel_x, accel_y, accel_z, "
                    "mic_raw, mic_p2p, mic_rms, latitude, longitude) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    new_rows,
                )

        acknowledgement = json.dumps(
            {"type": "ack_batch", "deviceId": device_id, "sequences": sequences},
            separators=(",", ":"),
        ).encode("utf-8")
        sock.sendto(acknowledgement, address)


class MonitorWindow(QMainWindow):
    """主窗口：呈现状态摘要、实时曲线、最近事件并导出历史数据。"""
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Gun Detection WiFi Monitor")
        self.resize(1280, 850)
        self.receiver = UdpReceiver()
        self.receiver.packet_received.connect(self.on_packet)
        self.receiver.receiver_error.connect(self.show_error)
        self.receiver.listening.connect(self.on_listening)
        self.samples = deque(maxlen=20000)
        self.x_values = deque(maxlen=20000)
        self.start_uptime = None
        self.last_packet_time = None
        self.shot_count = 0
        self.detection_counts = {"voice_count": 0, "vibration_count": 0}
        self._build_ui()
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.refresh_display)
        self.timer.start(UI_REFRESH_MS)

    def _build_ui(self):
        """创建连接控制栏、状态卡片和传感器曲线。"""
        root = QWidget()
        main = QVBoxLayout(root)

        connection = QHBoxLayout()
        connection.addWidget(QLabel("监听地址"))
        self.host_edit = QLineEdit("10.126.104.83")
        self.host_edit.setMaximumWidth(150)
        connection.addWidget(self.host_edit)
        connection.addWidget(QLabel("端口"))
        self.port_box = QSpinBox()
        self.port_box.setRange(1, 65535)
        self.port_box.setValue(8888)
        connection.addWidget(self.port_box)
        self.start_button = QPushButton("开始监听")
        self.start_button.clicked.connect(self.toggle_listening)
        connection.addWidget(self.start_button)
        self.save_button = QPushButton("导出 CSV")
        self.save_button.clicked.connect(self.export_csv)
        connection.addWidget(self.save_button)
        self.clear_button = QPushButton("清空曲线")
        self.clear_button.clicked.connect(self.clear_history)
        connection.addWidget(self.clear_button)
        self.connection_label = QLabel("尚未监听")
        connection.addWidget(self.connection_label, 1)
        main.addLayout(connection)

        summary = QGridLayout()
        self.values = {}
        fields = [
            ("device", "设备"), ("timestamp", "设备时间戳"),
            ("rssi", "WiFi 信号"), ("shots", "融合射击次数"),
            ("detection_counts", "检测次数"),
            ("flags", "检测标志"), ("gps_location", "GPS 定位"),
            ("gps_details", "GPS 信息"), ("storage_test", "Flash 容量测试"),
            ("mic", "麦克风"), ("accel", "三轴原始值"),
            ("tilt", "静态倾斜角"),
            ("vibration", "振动量 / 变化量"),
            ("voice_event", "最近声音事件"), ("vibration_event", "最近振动事件"),
            ("last_shot", "最近射击事件"), ("last_seen", "最近数据"),
        ]
        for index, (key, title) in enumerate(fields):
            box = QGroupBox(title)
            layout = QVBoxLayout(box)
            if key == "detection_counts":
                count_layout = QHBoxLayout()
                for value_key, initial in (
                    ("voice_count", "声音：0 次"),
                    ("vibration_count", "振动：0 次"),
                ):
                    label = QLabel(initial)
                    label.setTextInteractionFlags(Qt.TextSelectableByMouse)
                    count_layout.addWidget(label)
                    self.values[value_key] = label
                layout.addLayout(count_layout)
            elif key == "gps_location":
                gps_location_layout = QHBoxLayout()
                for value_key, initial in (
                    ("gps", "GPS：—"),
                    ("coordinates", "经纬度：—"),
                ):
                    label = QLabel(initial)
                    label.setTextInteractionFlags(Qt.TextSelectableByMouse)
                    label.setWordWrap(True)
                    gps_location_layout.addWidget(label)
                    self.values[value_key] = label
                layout.addLayout(gps_location_layout)
            elif key == "gps_details":
                gps_layout = QHBoxLayout()
                for value_key, initial in (
                    ("altitude", "海拔：— m"),
                    ("speed", "速度：— km/h"),
                    ("satellites", "卫星数：—"),
                ):
                    label = QLabel(initial)
                    label.setTextInteractionFlags(Qt.TextSelectableByMouse)
                    gps_layout.addWidget(label)
                    self.values[value_key] = label
                layout.addLayout(gps_layout)
            else:
                initial = "声音事件：—" if key == "voice_event" else (
                    "振动事件：—" if key == "vibration_event" else "—"
                )
                label = QLabel(initial)
                label.setTextInteractionFlags(Qt.TextSelectableByMouse)
                label.setWordWrap(True)
                layout.addWidget(label)
                self.values[key] = label
            summary.addWidget(box, index // 4, index % 4)
        main.addLayout(summary)

        self.plot_widget = pg.GraphicsLayoutWidget()
        self.plot_widget.setBackground("w")
        main.addWidget(self.plot_widget, 1)
        self.accel_plot = self.plot_widget.addPlot(row=0, col=0, title="三轴加速度原始 ADC 值")
        self.accel_plot.setLabel("bottom", "时间", units="s")
        self.accel_plot.showGrid(x=True, y=True, alpha=0.25)
        # 将颜色说明和可见性开关叠放在图表右上角，复选框控制对应曲线。
        self.accel_curves = [
            self.accel_plot.plot(pen=pg.mkPen("#d62728", width=1.5)),
            self.accel_plot.plot(pen=pg.mkPen("#2ca02c", width=1.5)),
            self.accel_plot.plot(pen=pg.mkPen("#1f77b4", width=1.5)),
        ]
        legend_panel = QWidget(self.plot_widget.viewport())
        legend_layout = QVBoxLayout(legend_panel)
        legend_layout.setContentsMargins(8, 5, 8, 5)
        legend_layout.setSpacing(2)
        legend_panel.setStyleSheet(
            "QWidget { background-color: rgba(255, 255, 255, 220); "
            "border: 1px solid #bbbbbb; border-radius: 4px; }"
        )
        self.axis_checks = []
        axis_items = [("X  红色", "#d62728"), ("Y  绿色", "#2ca02c"), ("Z  蓝色", "#1f77b4")]
        for curve, (label, color) in zip(self.accel_curves, axis_items):
            check = QCheckBox(label, legend_panel)
            check.setChecked(True)
            check.setStyleSheet(f"QCheckBox {{ color: {color}; font-weight: bold; }}")
            check.toggled.connect(curve.setVisible)
            legend_layout.addWidget(check)
            self.axis_checks.append(check)
        legend_panel.adjustSize()
        self.legend_panel = legend_panel
        self.plot_widget.viewport().installEventFilter(self)
        self._position_legend()
        main.setContentsMargins(8, 8, 8, 8)
        self.setCentralWidget(root)

    def _position_legend(self):
        """将轴颜色图例固定在绘图区域右上角。"""
        if not hasattr(self, "legend_panel"):
            return
        margin = 14
        self.legend_panel.move(
            self.plot_widget.viewport().width() - self.legend_panel.width() - margin,
            margin,
        )

    def resizeEvent(self, event):
        """窗口尺寸变化时同步移动右上角图例。"""
        super().resizeEvent(event)
        self._position_legend()

    def eventFilter(self, watched, event):
        """绘图区视口尺寸改变时重新定位图例。"""
        if watched is self.plot_widget.viewport() and event.type() == QEvent.Resize:
            self._position_legend()
        return super().eventFilter(watched, event)

    def toggle_listening(self):
        """根据当前接收线程状态开始或停止监听。"""
        if self.receiver._thread and self.receiver._thread.is_alive() and not self.receiver._stop.is_set():
            self.receiver.stop()
            self.start_button.setText("开始监听")
            return
        self.receiver.start(self.host_edit.text().strip() or "0.0.0.0", self.port_box.value())

    def on_listening(self, active, message):
        self.connection_label.setText(message)
        self.start_button.setText("停止监听" if active else "开始监听")

    def on_packet(self, packet):
        """记录收到数据的时间，并按协议类型分发状态包或射击事件包。"""
        self.last_packet_time = time.time()
        kind = packet.get("type", "?")
        device_timestamp = packet.get("timestamp") if packet.get("timestampValid", False) else None
        self.values["timestamp"].setText(device_timestamp or "未校准")
        self.values["last_seen"].setText(
            f"{device_timestamp or datetime.now().strftime('%Y-%m-%d %H:%M:%S')}  "
            f"{packet.get('_sender', '')}  [{kind}]"
        )
        if kind == "status":
            self.handle_status(packet)
        elif kind == "shot":
            self.handle_shot(packet)
        elif kind == "detection":
            self.handle_detection(packet)

    @staticmethod
    def number(value, digits=1):
        """将数值格式化为指定精度；缺失或非法值显示为占位符。"""
        try:
            return f"{float(value):.{digits}f}"
        except (TypeError, ValueError):
            return "—"

    def handle_status(self, packet):
        """更新实时状态卡片，并将状态样本存入曲线缓存和 CSV 缓存。"""
        device_timestamp = packet.get("timestamp") if packet.get("timestampValid", False) else None
        mic = packet.get("mic") or {}
        vibration = packet.get("vibration") or {}
        tilt = packet.get("tilt") or {}
        gps = packet.get("gps") or {}
        storage_test = packet.get("storageTest") or {}
        flags = packet.get("flags") or {}
        uptime = packet.get("uptimeMs")
        try:
            uptime = float(uptime)
        except (TypeError, ValueError):
            uptime = time.monotonic() * 1000
        if self.start_uptime is None:
            self.start_uptime = uptime
        # 以设备运行时间为横轴，避免电脑本地时钟变化影响曲线连续性。
        elapsed = max(0.0, (uptime - self.start_uptime) / 1000.0)

        row = {
            "received_at": device_timestamp or datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "uptime_ms": uptime,
            "device_id": packet.get("deviceId", ""),
            "rssi": packet.get("rssi", ""),
            "shot_count": packet.get("shotCount", ""),
            "voice_detection_count": packet.get("voiceDetectionCount", ""),
            "vibration_detection_count": packet.get("vibrationDetectionCount", ""),
            "mic_raw": mic.get("raw", ""), "mic_p2p": mic.get("p2p", ""),
            "mic_rms": mic.get("rms", ""),
            "vibration_x": vibration.get("x", ""),
            "vibration_y": vibration.get("y", ""),
            "vibration_z": vibration.get("z", ""),
            "vibration_magnitude": vibration.get("magnitude", ""),
            "vibration_jerk": vibration.get("jerk", ""),
            "roll_deg": tilt.get("rollDeg", ""),
            "pitch_deg": tilt.get("pitchDeg", ""),
            "gps_valid": gps.get("valid", ""),
            "latitude": gps.get("latitude", ""), "longitude": gps.get("longitude", ""),
            "altitude_m": gps.get("altitudeM", ""),
            "speed_kmph": gps.get("speedKmph", ""),
            "satellites": gps.get("satellites", ""),
        }
        self.samples.append(row)
        self.x_values.append(elapsed)
        self.values["device"].setText(str(packet.get("deviceId", "—")))
        self.values["rssi"].setText(f"{packet.get('rssi', '—')} dBm")
        self.values["shots"].setText(str(packet.get("shotCount", self.shot_count)))
        self.values["flags"].setText(
            f"声音：{'触发' if flags.get('voice') else '无'}；振动：{'触发' if flags.get('vibration') else '无'}"
        )
        valid = bool(gps.get("valid", False))
        self.values["gps"].setText(f"GPS：{'有效' if valid else '无有效定位'}")
        self.values["coordinates"].setText(
            f"经纬度：{self.number(gps.get('latitude'), 7)}, "
            f"{self.number(gps.get('longitude'), 7)}"
        )
        self.values["altitude"].setText(f"海拔：{self.number(gps.get('altitudeM'))} m")
        self.values["speed"].setText(f"速度：{self.number(gps.get('speedKmph'))} km/h")
        self.values["satellites"].setText(f"卫星数：{gps.get('satellites', '—')}")
        if not storage_test.get("enabled", False):
            self.values["storage_test"].setText("未启用")
        elif storage_test.get("full", False):
            self.values["storage_test"].setText(
                f"已存满：{storage_test.get('records', 0)} 条\n"
                f"占用：{storage_test.get('usedBytes', 0)}/"
                f"{storage_test.get('totalBytes', 0)} 字节"
            )
        else:
            self.values["storage_test"].setText(
                f"当前已存入：{storage_test.get('currentRecords', 0)} 条\n"
                "测试记录中，尚未存满"
            )
        self.values["mic"].setText(
            f"raw {mic.get('raw', '—')}   P2P {mic.get('p2p', '—')}   RMS {self.number(mic.get('rms'))}"
        )
        self.values["accel"].setText(
            f"X {vibration.get('x', '—')}   Y {vibration.get('y', '—')}   Z {vibration.get('z', '—')}"
        )
        if tilt.get("valid", False):
            self.values["tilt"].setText(
                f"横滚：{self.number(tilt.get('rollDeg'))}°   "
                f"俯仰：{self.number(tilt.get('pitchDeg'))}°"
            )
        else:
            self.values["tilt"].setText("校准中或角度无效")
        self.values["vibration"].setText(
            f"幅值 {self.number(vibration.get('magnitude'))}   变化 {self.number(vibration.get('jerk'))}"
        )
        self.shot_count = int(packet.get("shotCount", self.shot_count) or 0)
        self.update_detection_count("voice_count", packet.get("voiceDetectionCount", 0), "声音")
        self.update_detection_count("vibration_count", packet.get("vibrationDetectionCount", 0), "振动")
        if len(self.x_values) > 1:
            left = max(0.0, elapsed - HISTORY_SECONDS)
            self.accel_plot.setXRange(left, max(HISTORY_SECONDS, elapsed), padding=0)

    def handle_shot(self, packet):
        """更新射击计数和最近一次融合事件摘要。"""
        shot_id = packet.get("shotId", "?")
        self.shot_count = int(packet.get("shotCount", self.shot_count + 1) or 0)
        self.values["shots"].setText(str(self.shot_count))
        lat, lon = packet.get("latitude"), packet.get("longitude")
        location = f"GPS {self.number(lat, 7)}, {self.number(lon, 7)}" if packet.get("gpsValid") else "GPS 无效"
        timestamp = packet.get("timestamp") if packet.get("timestampValid", False) else "未校准"
        self.values["last_shot"].setText(
            f"{timestamp}  #{shot_id}  声音 P2P {packet.get('voiceP2P', '—')}  "
            f"振动峰值 {self.number(packet.get('accelPeak'))}  {location}"
        )

    def handle_detection(self, packet):
        """分别更新声音或振动检测累计数和最近一次独立触发事件。"""
        detection_type = packet.get("detectionType")
        count = int(packet.get("detectionCount", 0) or 0)
        timestamp = packet.get("timestamp") if packet.get("timestampValid", False) else "未校准"
        if detection_type == "voice":
            self.update_detection_count("voice_count", count, "声音")
            details = (
                f"P2P {packet.get('peakToPeak', '—')}，RMS {self.number(packet.get('rms'))}，"
                f"突增 {self.number(packet.get('riseRatio'), 2)}"
            )
            self.values["voice_event"].setText(
                f"声音事件：{timestamp}\n"
                f"#{count}  {details}"
            )
        elif detection_type == "vibration":
            self.update_detection_count("vibration_count", count, "振动")
            details = (
                f"峰值 {self.number(packet.get('peakMagnitude'))}，"
                f"变化 {self.number(packet.get('peakJerk'))}，"
                f"三轴 {packet.get('x', '—')}/{packet.get('y', '—')}/{packet.get('z', '—')}"
            )
            self.values["vibration_event"].setText(
                f"振动事件：{timestamp}\n"
                f"#{count}  {details}"
            )

    def update_detection_count(self, key, count, label):
        """避免延迟到达的旧事件包让界面上的累计次数倒退。"""
        current = self.detection_counts[key]
        self.detection_counts[key] = max(current, int(count or 0))
        self.values[key].setText(f"{label}：{self.detection_counts[key]} 次")

    def refresh_display(self):
        """由 Qt 定时器批量刷新曲线，并提示数据流中断。"""
        if not self.samples:
            return
        rows = list(self.samples)
        xs = list(self.x_values)
        self.accel_curves[0].setData(xs, [self.as_float(r["vibration_x"]) for r in rows])
        self.accel_curves[1].setData(xs, [self.as_float(r["vibration_y"]) for r in rows])
        self.accel_curves[2].setData(xs, [self.as_float(r["vibration_z"]) for r in rows])
        if self.last_packet_time and time.time() - self.last_packet_time > 3:
            self.connection_label.setText("已超过 3 秒未收到数据")

    @staticmethod
    def as_float(value):
        """将曲线字段转换为浮点数，缺失值按零显示。"""
        try:
            return float(value)
        except (TypeError, ValueError):
            return 0.0

    def clear_history(self):
        """清空本地缓存、重置时间原点并清除所有曲线。"""
        self.samples.clear()
        self.x_values.clear()
        self.start_uptime = None
        for curve in self.accel_curves:
            curve.clear()

    def export_csv(self):
        """将已缓存的状态包样本导出为带 BOM 的 UTF-8 CSV 文件。"""
        if not self.samples:
            QMessageBox.information(self, "没有数据", "收到状态数据后才能导出 CSV。")
            return
        default_name = f"gun_telemetry_{datetime.now():%Y%m%d_%H%M%S}.csv"
        path, _ = QFileDialog.getSaveFileName(self, "导出遥测数据", default_name, "CSV 文件 (*.csv)")
        if not path:
            return
        try:
            with open(path, "w", newline="", encoding="utf-8-sig") as file:
                writer = csv.DictWriter(file, fieldnames=CSV_FIELDS)
                writer.writeheader()
                writer.writerows(self.samples)
            self.connection_label.setText(f"已导出 {len(self.samples)} 条状态数据：{path}")
        except OSError as exc:
            QMessageBox.critical(self, "导出失败", str(exc))

    def show_error(self, message):
        """在窗口连接状态栏显示接收线程错误。"""
        self.connection_label.setText(message)

    def closeEvent(self, event):
        """关闭窗口前停止后台接收，避免线程遗留。"""
        self.receiver.stop()
        event.accept()


def main():
    """创建 Qt 应用并进入桌面事件循环。"""
    app = QApplication(sys.argv)
    window = MonitorWindow()
    window.show()
    sys.exit(app.exec_())


if __name__ == "__main__":
    main()
