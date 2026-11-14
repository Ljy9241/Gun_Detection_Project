#pragma once

#include <Arduino.h>

#ifndef GUN_CAPACITY_TEST_MODE
#define GUN_CAPACITY_TEST_MODE 0
#endif
#ifndef GUN_STANDBY_TEST_MODE
#define GUN_STANDBY_TEST_MODE 0
#endif

namespace config {

inline constexpr bool CAPACITY_TEST_MODE = GUN_CAPACITY_TEST_MODE != 0;

// 每日北京时间定时待机；只在已有有效 GPS/NTP 日历时间后启用。
inline constexpr bool NIGHT_STANDBY_ENABLED = true;//是否启用定时休眠
inline constexpr uint8_t NIGHT_STANDBY_END_HOUR = 23;       // 几点工作：小时 0~23。
inline constexpr uint8_t NIGHT_STANDBY_END_MIN = 00;        // 几点工作：分钟 0~59。
inline constexpr uint8_t NIGHT_STANDBY_START_HOUR = 11;    // 几点休眠：小时 0~23。
inline constexpr uint8_t NIGHT_STANDBY_START_MIN = 40;      // 几点休眠：分钟 0~59。
static_assert(NIGHT_STANDBY_START_HOUR < 24 && NIGHT_STANDBY_END_HOUR < 24 &&
              NIGHT_STANDBY_START_MIN < 60 && NIGHT_STANDBY_END_MIN < 60,
              "Standby hours must be 0-23 and minutes 0-59");
inline constexpr uint16_t NIGHT_STANDBY_START_MINUTE = NIGHT_STANDBY_START_HOUR * 60 + NIGHT_STANDBY_START_MIN;
inline constexpr uint16_t NIGHT_STANDBY_END_MINUTE = NIGHT_STANDBY_END_HOUR * 60 + NIGHT_STANDBY_END_MIN;
static_assert(NIGHT_STANDBY_START_MINUTE < 1440 && NIGHT_STANDBY_END_MINUTE < 1440 &&
              NIGHT_STANDBY_START_MINUTE != NIGHT_STANDBY_END_MINUTE,
              "Daily standby needs distinct start/end minutes within a day");

// 测试无限续航目标启用：每次上电只运行一次，完成后一直工作，忽略每日待机时段。
inline constexpr bool STANDBY_TEST_ENABLED = GUN_STANDBY_TEST_MODE != 0;
inline constexpr uint16_t STANDBY_TEST_DELAY_MINUTES = 1;  // 上电工作几分钟后休眠。
inline constexpr uint16_t STANDBY_TEST_SLEEP_MINUTES = 1;  // 休眠几分钟后恢复。
static_assert(!STANDBY_TEST_ENABLED || (!CAPACITY_TEST_MODE && STANDBY_TEST_SLEEP_MINUTES > 0),
              "Standby test requires a positive sleep duration and cannot be a capacity test");

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
inline constexpr uint16_t VOICE_P2P_THRESHOLD = 150;
inline constexpr float VOICE_RMS_THRESHOLD = 50.0F;
inline constexpr float VOICE_RISE_RATIO = 1.5F;
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

// 射击记录改由横向动作触发；方向识别阈值在 motion_direction.h 中配置。

// Wi-Fi 热点配置；电脑地址通过局域网 UDP 自动发现。
inline constexpr char WIFI_SSID[] = "ljy";
inline constexpr char WIFI_PASSWORD[] = "zxcvbnm1";
inline constexpr uint16_t MONITOR_DISCOVERY_PORT = 8888; // 必须与上位机监听端口一致，无需配置电脑 IP。
inline constexpr uint32_t MONITOR_DISCOVERY_INTERVAL_MS = 2000;
inline constexpr uint32_t MONITOR_DISCOVERY_TIMEOUT_MS = 8000;
inline constexpr uint16_t DEVICE_UDP_PORT = 8889;  // 接收电脑端入库确认（ACK）。
inline constexpr char NTP_SERVER[] = "pool.ntp.org";
inline constexpr char DEVICE_ID[] = "gun_dection";
// 普通模式：true 上电自动联网；false 等待完整竖向动作后临时联网。
inline constexpr bool NORMAL_WIFI_ALWAYS_ON = true;
inline constexpr uint32_t NORMAL_WIFI_HOLD_MS = 120000;          // 首次连上热点后保持的毫秒数（默认 2 分钟）。
inline constexpr uint32_t NORMAL_WIFI_CONNECT_TIMEOUT_MS = 60000; // 未能连上热点时最多尝试 1 分钟。
static_assert(NORMAL_WIFI_HOLD_MS > 0 && NORMAL_WIFI_CONNECT_TIMEOUT_MS > 0,
              "Normal Wi-Fi window durations must be positive");
inline constexpr uint32_t WIFI_RECONNECT_INTERVAL_MS = 30000;      // 连接失败后再尝试的最短间隔；避免打断首次连接
inline constexpr uint32_t NORMAL_GPS_TRACK_INTERVAL_MS = 1000;       // 普通模式 GPS 常开时的轨迹保存间隔，毫秒。
// 以下两项分别控制普通/低功耗模式的声音和三轴振动数据保存周期。
// 单位为毫秒：10000 = 每 10 秒保存一次；只影响保存，不改变每路 1 kHz 连续采样识别。
inline constexpr uint32_t NORMAL_SENSOR_RECORD_INTERVAL_MS = 10000;    // 普通模式保存间隔，可单独修改。
inline constexpr uint32_t LOW_POWER_SENSOR_RECORD_INTERVAL_MS = 10000; // 低功耗模式保存间隔，可单独修改。
static_assert(NORMAL_GPS_TRACK_INTERVAL_MS > 0 && NORMAL_SENSOR_RECORD_INTERVAL_MS > 0 &&
              LOW_POWER_SENSOR_RECORD_INTERVAL_MS > 0, "Recording intervals must be positive");
// 内部计算值，无需另行配置；容量测试仍为 50 ms，日常普通模式使用上面的保存间隔。
inline constexpr uint32_t TELEMETRY_SPOOL_INTERVAL_MS = CAPACITY_TEST_MODE ? 50 : NORMAL_SENSOR_RECORD_INTERVAL_MS;
inline constexpr uint8_t TELEMETRY_FLASH_BATCH_RECORDS = 20;      // RAM 累积 20 条后一次写入 Flash；断电最多损失约 20 条。
inline constexpr uint32_t UDP_STATUS_INTERVAL_MS = 250;           // UDP 状态包每秒发送 4 次；降低主循环及 LittleFS 状态查询负担。
inline constexpr uint32_t UDP_EVENT_RETRY_INTERVAL_MS = 100;      // 发送失败的事件至少间隔 100 毫秒重试，避免 UDP 错误时忙循环。
inline constexpr uint32_t UDP_TELEMETRY_RETRY_INTERVAL_MS = 500;  // 遥测批次失败或等待 ACK 时的重试间隔，单位：毫秒。
inline constexpr float LOW_POWER_MOTION_DELTA_THRESHOLD_COUNTS = 1.0F; // 相邻采样任意单轴变化绝对值超过此值视为运动，单位：ADC counts。
inline constexpr uint32_t LOW_POWER_STILLNESS_MS = 5000;              // 静止持续此时长后进入低功耗，单位：毫秒。
inline constexpr bool NORMAL_GPS_ALWAYS_ON = true;                 // 普通模式：true GPS 常开；false 按下方周期启停。低功耗始终按周期启停。
inline constexpr uint32_t GPS_CYCLE_MS = 600000;                     // 周期模式下 GPS 启动间隔 10 分钟。
inline constexpr uint32_t GPS_WINDOW_MS = 180000;                    // 周期模式下单次定位窗口最长 3 分钟；普通模式常开时不关闭 GPS。
inline constexpr uint32_t GPS_FLASH_RETRY_MS = 1000;                 // 快照写入失败时每秒重试，不忙循环。
inline constexpr bool LOW_POWER_WIFI_CYCLE_ENABLED = false;           // 控制低功耗是否定时联网；不影响竖向动作联网。
inline constexpr uint32_t LOW_POWER_VERTICAL_WIFI_HOLD_MS = 10000;   // 低功耗竖向动作连上热点后保持 2 分钟。
static_assert(LOW_POWER_VERTICAL_WIFI_HOLD_MS > 0, "Low-power gesture Wi-Fi duration must be positive");
inline constexpr uint32_t LOW_POWER_WIFI_CYCLE_MS = 600000;           // WiFi 重复连接周期，单位：毫秒（10 分钟）。
inline constexpr uint32_t LOW_POWER_WIFI_WINDOW_MS = 300000;           // 每个周期内 WiFi 保持开启的时长，单位：毫秒（5 分钟）。

// 串口诊断信息的输出周期。
inline constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 10000;

}  // namespace config
