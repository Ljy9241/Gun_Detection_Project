#pragma once

#include <Arduino.h>

#ifndef GUN_CAPACITY_TEST_MODE
#define GUN_CAPACITY_TEST_MODE 0
#endif

namespace config {

inline constexpr bool CAPACITY_TEST_MODE = GUN_CAPACITY_TEST_MODE != 0;

// 硬件引脚定义（ESP32-S3）。
inline constexpr uint8_t MIC_ADC_PIN = 8;       // ADC1_CH7
inline constexpr uint8_t MMA_X_PIN = 4;         // ADC1_CH3
inline constexpr uint8_t MMA_Y_PIN = 5;         // ADC1_CH4
inline constexpr uint8_t MMA_Z_PIN = 6;         // ADC1_CH5
inline constexpr uint8_t MMA_SLEEP_PIN = 7;     // HIGH: active, LOW: sleep
inline constexpr uint8_t GPS_ENABLE_PIN = 9;    // Assumes HIGH enables the module
inline constexpr uint8_t GPS_PPS_PIN = 10;
inline constexpr uint8_t GPS_UART_TX_PIN = 17;  // ESP32 TX -> GPS RX
inline constexpr uint8_t GPS_UART_RX_PIN = 18;  // ESP32 RX <- GPS TX

inline constexpr uint32_t SERIAL_BAUD = 115200;
inline constexpr uint32_t GPS_BAUD = 9600;
inline constexpr uint32_t GPS_MAX_FIX_AGE_MS = 5000;

// 声音检测参数：当前为初始值，实际使用前应结合采集数据校准。
inline constexpr uint32_t VOICE_SAMPLE_RATE_HZ = 1000;
inline constexpr uint16_t VOICE_FRAME_SAMPLES = 10;  // 10 ms at 1 kHz
inline constexpr uint16_t VOICE_P2P_THRESHOLD = 20;
inline constexpr float VOICE_RMS_THRESHOLD = 3.0F;
inline constexpr float VOICE_RISE_RATIO = 1.0F;
inline constexpr uint32_t VOICE_RETRIGGER_US = 50000;

// MMA7361 振动检测参数；PCB 将 G-SELECT 拉高，因此量程为 +/-6 g。
inline constexpr uint32_t VIBRATE_SAMPLE_RATE_HZ = 1000;
inline constexpr uint16_t VIBRATE_CALIBRATION_SAMPLES = 300;
inline constexpr uint16_t TILT_CALIBRATION_SAMPLES = 2000;  // 2 s stationary startup calibration
inline constexpr float VIBRATE_PEAK_THRESHOLD_COUNTS = 300.0F;
inline constexpr float VIBRATE_JERK_THRESHOLD_COUNTS = 150.0F;
inline constexpr float VIBRATE_QUIET_THRESHOLD_COUNTS = 90.0F;
inline constexpr uint8_t VIBRATE_TRIGGER_SAMPLES = 2;
inline constexpr uint32_t VIBRATE_RETRIGGER_US = 50000;

// 声音与振动事件的融合窗口及融合后的重复触发冷却时间。
inline constexpr uint32_t FUSION_WINDOW_US = 100000;
inline constexpr uint32_t SHOT_COOLDOWN_US = 150000;

// Wi-Fi 与上位机配置；进行网络联调前请填写实际热点和电脑地址。
inline constexpr char WIFI_SSID[] = "ljy";
inline constexpr char WIFI_PASSWORD[] = "zxcvbnm1";
// 此处填写电脑无线网卡的 IPv4 地址，不要填写手机热点网关地址。
inline constexpr char UPPER_COMPUTER_HOST[] = "10.126.104.83";
inline constexpr uint16_t UPPER_COMPUTER_PORT = 8888;
inline constexpr uint16_t DEVICE_UDP_PORT = 8889;  // 接收电脑端入库确认（ACK）。
inline constexpr char NTP_SERVER[] = "pool.ntp.org";
inline constexpr char DEVICE_ID[] = "gun_dection";
inline constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;      // 连接失败后再尝试的最短间隔；避免打断首次连接
inline constexpr uint32_t TELEMETRY_SPOOL_INTERVAL_MS = 50;       // 遥测记录写入 Flash 的目标间隔
inline constexpr uint8_t TELEMETRY_FLASH_BATCH_RECORDS = 20;      // RAM 累积 20 条后一次写入 Flash；断电最多损失约 20 条。
inline constexpr uint32_t UDP_STATUS_INTERVAL_MS = 250;           // UDP 状态包每秒发送 4 次；降低主循环及 LittleFS 状态查询负担。
inline constexpr uint32_t UDP_EVENT_RETRY_INTERVAL_MS = 100;      // 发送失败的事件至少间隔 100 毫秒重试，避免 UDP 错误时忙循环。
inline constexpr uint32_t UDP_TELEMETRY_RETRY_INTERVAL_MS = 500;  // 遥测批次失败或等待 ACK 时的重试间隔，单位：毫秒。
inline constexpr float LOW_POWER_MOTION_DELTA_THRESHOLD_COUNTS = 10.0F; // 相邻三轴样本向量差低于此值视为静止，单位：ADC counts。
inline constexpr uint32_t LOW_POWER_STILLNESS_MS = 5000;              // 静止持续此时长后进入低功耗，单位：毫秒。
inline constexpr uint32_t LOW_POWER_TELEMETRY_INTERVAL_MS = 10000;    // 低功耗期间两条遥测记录的写入间隔，单位：毫秒。
inline constexpr uint32_t LOW_POWER_GPS_CYCLE_MS = 600000;            // GPS 重复启动周期，单位：毫秒（10 分钟）。
inline constexpr uint32_t LOW_POWER_GPS_WINDOW_MS = 300000;            // GPS 未取得有效定位时的最长工作时间，单位：毫秒（5 分钟）。
inline constexpr uint32_t LOW_POWER_GPS_POST_FIX_HOLD_MS = 30000;      // GPS 首次取得有效定位后继续开启的时长，单位：毫秒（30 秒）。
inline constexpr uint32_t LOW_POWER_WIFI_CYCLE_MS = 600000;           // WiFi 重复连接周期，单位：毫秒（10 分钟）。
inline constexpr uint32_t LOW_POWER_WIFI_WINDOW_MS = 300000;           // 每个周期内 WiFi 保持开启的时长，单位：毫秒（5 分钟）。

// 串口诊断信息的输出周期。
inline constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 500;

}  // namespace config
