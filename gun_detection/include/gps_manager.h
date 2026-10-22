#pragma once

#include <Arduino.h>
#include <TinyGPSPlus.h>

// 一次 GPS 定位快照；valid 同时考虑定位有效性和数据新鲜度。
struct GpsFix {
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeM = 0.0;
    double speedKmph = 0.0;
    double hdop = 0.0;
    uint32_t satellites = 0;
    uint32_t ageMs = UINT32_MAX;
};

class GpsManager {
public:
    // serial 由调用方提供，便于选择硬件串口并复用其生命周期。
    explicit GpsManager(HardwareSerial& serial) : serial_(serial) {}

    void begin(uint8_t enablePin, uint8_t ppsPin, int8_t rxPin, int8_t txPin, uint32_t baud);
    void update();  // 持续读取串口字节并交给 TinyGPSPlus 解析。
    GpsFix currentFix();  // 将解析器中的最新字段整理成快照。
    uint32_t parsedCharacters() const { return gps_.charsProcessed(); }
    uint32_t failedChecksums() const { return gps_.failedChecksum(); }

private:
    HardwareSerial& serial_;
    TinyGPSPlus gps_;
    uint8_t enablePin_ = 0;
    uint8_t ppsPin_ = 0;
};
