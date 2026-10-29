#pragma once

#include <Arduino.h>

enum class AdcInput : uint8_t {
    Microphone,
    AccelX,
    AccelY,
    AccelZ,
};

struct AdcSample {
    AdcInput input;
    uint16_t value;
    uint64_t timestampUs;
};

// ESP32-S3 ADC1 continuous/DMA sampler. The conversion pattern scans the
// microphone and three accelerometer inputs at 1 kHz per input.
class AdcSampler {
public:
    bool begin();
    bool start();
    void stop();
    size_t read(AdcSample* samples, size_t capacity, uint64_t newestSampleUs);
    uint32_t overrunCount() const { return overrunCount_; }
    bool ready() const { return ready_; }

private:
    volatile bool ready_ = false;
    volatile bool running_ = false;
    volatile uint32_t overrunCount_ = 0;
    uint64_t lastSampleTimestampUs_ = 0;
    uint8_t dmaBytes_[4096] = {};
};
