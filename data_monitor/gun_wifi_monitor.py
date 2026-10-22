"""ESP32-S3 检测设备的 UDP/JSON 上位机：实时显示遥测数据并支持 CSV 导出。"""

import csv
import json
import socket
import sys
import threading
import time
from collections import deque
from datetime import datetime

import pyqtgraph as pg
from PyQt5.QtCore import QObject, QTimer, Qt, pyqtSignal
from PyQt5.QtWidgets import (
    QApplication,
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
    "mic_raw", "mic_p2p", "mic_rms", "vibration_x", "vibration_y",
    "vibration_z", "vibration_magnitude", "vibration_jerk", "gps_valid",
    "latitude", "longitude", "altitude_m", "speed_kmph", "satellites",
]


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
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind((host, port))
            sock.settimeout(0.5)
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
                        self.packet_received.emit(packet)
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    self.receiver_error.emit(f"收到非 JSON 数据：{exc}")
            self.listening.emit(False, "已停止监听")
        except OSError as exc:
            self.listening.emit(False, "监听失败")
            self.receiver_error.emit(f"无法监听 {host}:{port}：{exc}")
        finally:
            try:
                sock.close()
            except OSError:
                pass
            if self._socket is sock:
                self._socket = None


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
            ("device", "设备"), ("rssi", "WiFi 信号"), ("shots", "检测次数"),
            ("flags", "检测标志"), ("gps", "GPS 状态"),
            ("coordinates", "经纬度"), ("altitude", "海拔"),
            ("speed", "速度"), ("satellites", "卫星数"),
            ("mic", "麦克风"), ("accel", "三轴原始值"),
            ("vibration", "振动量 / 变化量"),
            ("last_shot", "最近事件"), ("last_seen", "最近数据"),
        ]
        for index, (key, title) in enumerate(fields):
            box = QGroupBox(title)
            layout = QVBoxLayout(box)
            label = QLabel("—")
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
        self.accel_curves = [
            self.accel_plot.plot(pen=pg.mkPen("#d62728", width=1.5), name="X"),
            self.accel_plot.plot(pen=pg.mkPen("#2ca02c", width=1.5), name="Y"),
            self.accel_plot.plot(pen=pg.mkPen("#1f77b4", width=1.5), name="Z"),
        ]
        self.accel_plot.addLegend()

        self.mic_plot = self.plot_widget.addPlot(row=1, col=0, title="声音与振动指标")
        self.mic_plot.setLabel("bottom", "时间", units="s")
        self.mic_plot.showGrid(x=True, y=True, alpha=0.25)
        self.mic_curves = [
            self.mic_plot.plot(pen=pg.mkPen("#9467bd", width=1.5), name="Mic P2P"),
            self.mic_plot.plot(pen=pg.mkPen("#ff7f0e", width=1.5), name="Mic RMS"),
            self.mic_plot.plot(pen=pg.mkPen("#17becf", width=1.5), name="振动幅值"),
        ]
        self.mic_plot.addLegend()
        self.mic_plot.setXLink(self.accel_plot)
        main.setContentsMargins(8, 8, 8, 8)
        self.setCentralWidget(root)

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
        self.values["last_seen"].setText(
            f"{datetime.now().strftime('%H:%M:%S')}  {packet.get('_sender', '')}  [{kind}]"
        )
        if kind == "status":
            self.handle_status(packet)
        elif kind == "shot":
            self.handle_shot(packet)

    @staticmethod
    def number(value, digits=1):
        """将数值格式化为指定精度；缺失或非法值显示为占位符。"""
        try:
            return f"{float(value):.{digits}f}"
        except (TypeError, ValueError):
            return "—"

    def handle_status(self, packet):
        """更新实时状态卡片，并将状态样本存入曲线缓存和 CSV 缓存。"""
        mic = packet.get("mic") or {}
        vibration = packet.get("vibration") or {}
        gps = packet.get("gps") or {}
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
            "received_at": datetime.now().isoformat(timespec="milliseconds"),
            "uptime_ms": uptime,
            "device_id": packet.get("deviceId", ""),
            "rssi": packet.get("rssi", ""),
            "shot_count": packet.get("shotCount", ""),
            "mic_raw": mic.get("raw", ""), "mic_p2p": mic.get("p2p", ""),
            "mic_rms": mic.get("rms", ""),
            "vibration_x": vibration.get("x", ""),
            "vibration_y": vibration.get("y", ""),
            "vibration_z": vibration.get("z", ""),
            "vibration_magnitude": vibration.get("magnitude", ""),
            "vibration_jerk": vibration.get("jerk", ""),
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
        self.values["gps"].setText("有效" if valid else "无有效定位")
        self.values["coordinates"].setText(
            f"{self.number(gps.get('latitude'), 7)}, {self.number(gps.get('longitude'), 7)}"
        )
        self.values["altitude"].setText(f"{self.number(gps.get('altitudeM'))} m")
        self.values["speed"].setText(f"{self.number(gps.get('speedKmph'))} km/h")
        self.values["satellites"].setText(str(gps.get("satellites", "—")))
        self.values["mic"].setText(
            f"raw {mic.get('raw', '—')}   P2P {mic.get('p2p', '—')}   RMS {self.number(mic.get('rms'))}"
        )
        self.values["accel"].setText(
            f"X {vibration.get('x', '—')}   Y {vibration.get('y', '—')}   Z {vibration.get('z', '—')}"
        )
        self.values["vibration"].setText(
            f"幅值 {self.number(vibration.get('magnitude'))}   变化 {self.number(vibration.get('jerk'))}"
        )
        self.shot_count = int(packet.get("shotCount", self.shot_count) or 0)
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
        self.values["last_shot"].setText(
            f"#{shot_id}  声音 P2P {packet.get('voiceP2P', '—')}  "
            f"振动峰值 {self.number(packet.get('accelPeak'))}  {location}"
        )

    def refresh_display(self):
        """由 Qt 定时器批量刷新曲线，并提示数据流中断。"""
        if not self.samples:
            return
        rows = list(self.samples)
        xs = list(self.x_values)
        self.accel_curves[0].setData(xs, [self.as_float(r["vibration_x"]) for r in rows])
        self.accel_curves[1].setData(xs, [self.as_float(r["vibration_y"]) for r in rows])
        self.accel_curves[2].setData(xs, [self.as_float(r["vibration_z"]) for r in rows])
        self.mic_curves[0].setData(xs, [self.as_float(r["mic_p2p"]) for r in rows])
        self.mic_curves[1].setData(xs, [self.as_float(r["mic_rms"]) for r in rows])
        self.mic_curves[2].setData(xs, [self.as_float(r["vibration_magnitude"]) for r in rows])
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
        for curve in self.accel_curves + self.mic_curves:
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
