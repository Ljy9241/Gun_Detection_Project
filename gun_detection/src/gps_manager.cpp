#include "gps_manager.h"

#include <sys/time.h>
#include <time.h>

#include "system_config.h"
#include "gps_clock_policy.h"

void GpsManager::begin(uint8_t enablePin, uint8_t ppsPin, int8_t rxPin, int8_t txPin,
                       uint32_t baud) {
    enablePin_ = enablePin;
    ppsPin_ = ppsPin;
    rxPin_ = rxPin;
    txPin_ = txPin;
    baud_ = baud;
    pinMode(enablePin_, OUTPUT);
    digitalWrite(enablePin_, HIGH);  // 当前硬件配置为高电平使能 GPS。
    pinMode(ppsPin_, INPUT);
    serial_.begin(baud, SERIAL_8N1, rxPin, txPin);
    enabled_ = true;
}

void GpsManager::setEnabled(bool enabled) {
    if (enabled == enabled_) return;
    if (enabled) {
        // A new window must acquire fresh NMEA, never accept the previous fix.
        gps_ = TinyGPSPlus{};
        digitalWrite(enablePin_, HIGH);
        serial_.begin(baud_, SERIAL_8N1, rxPin_, txPin_);
    } else {
        serial_.end();
        digitalWrite(enablePin_, LOW);
    }
    enabled_ = enabled;
}

void GpsManager::update() {
    if (!enabled_) return;
    // 尽可能清空串口接收缓冲区，避免主循环期间 NMEA 数据积压。
    while (serial_.available() > 0) {
        gps_.encode(static_cast<char>(serial_.read()));
    }

    // GPS 日期/时间为 UTC；用有效 NMEA 时间校准 ESP32 的 RTC 系统时钟。
    if (gps_.date.isValid() && gps_.time.isValid() && gps_.date.age() <= 5000 &&
        gps_.time.age() <= 2000) {
        int64_t candidate = 0;
        const unsigned centisecond = gps_.time.centisecond();
        if (!gps_clock::utcEpoch(gps_.date.year(), gps_.date.month(), gps_.date.day(),
                                 gps_.time.hour(), gps_.time.minute(), gps_.time.second(),
                                 centisecond, candidate) ||
            !gps_clock::maySync(candidate, static_cast<int64_t>(time(nullptr)))) return;
        const time_t epoch = static_cast<time_t>(candidate);
        if (static_cast<int64_t>(epoch) != candidate) return;
        if (static_cast<uint32_t>(epoch) != lastClockSyncEpoch_) {
            timeval value = {epoch, static_cast<suseconds_t>(centisecond * 10000)};
            if (settimeofday(&value, nullptr) == 0) {
                lastClockSyncEpoch_ = static_cast<uint32_t>(epoch);
            }
        }
    }
}

GpsFix GpsManager::currentFix() {
    GpsFix fix;
    // 定位字段可能仍有旧值；只有坐标有效且未超过允许年龄时才标记为有效。
    fix.valid = enabled_ && gps_.location.isValid() && gps_.location.age() <= config::GPS_MAX_FIX_AGE_MS;
    if (gps_.location.isValid()) {
        fix.latitude = gps_.location.lat();
        fix.longitude = gps_.location.lng();
        fix.ageMs = gps_.location.age();
    }
    if (gps_.altitude.isValid()) {
        fix.altitudeM = gps_.altitude.meters();
    }
    if (gps_.speed.isValid()) {
        fix.speedKmph = gps_.speed.kmph();
    }
    if (gps_.hdop.isValid()) {
        fix.hdop = gps_.hdop.hdop();
    }
    if (gps_.satellites.isValid()) {
        fix.satellites = gps_.satellites.value();
    }
    return fix;
}
