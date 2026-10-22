#include "gps_manager.h"

#include "system_config.h"

void GpsManager::begin(uint8_t enablePin, uint8_t ppsPin, int8_t rxPin, int8_t txPin,
                       uint32_t baud) {
    enablePin_ = enablePin;
    ppsPin_ = ppsPin;
    pinMode(enablePin_, OUTPUT);
    digitalWrite(enablePin_, HIGH);  // 当前硬件配置为高电平使能 GPS。
    pinMode(ppsPin_, INPUT);
    serial_.begin(baud, SERIAL_8N1, rxPin, txPin);
}

void GpsManager::update() {
    // 尽可能清空串口接收缓冲区，避免主循环期间 NMEA 数据积压。
    while (serial_.available() > 0) {
        gps_.encode(static_cast<char>(serial_.read()));
    }
}

GpsFix GpsManager::currentFix() {
    GpsFix fix;
    // 定位字段可能仍有旧值；只有坐标有效且未超过允许年龄时才标记为有效。
    fix.valid = gps_.location.isValid() && gps_.location.age() <= config::GPS_MAX_FIX_AGE_MS;
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
