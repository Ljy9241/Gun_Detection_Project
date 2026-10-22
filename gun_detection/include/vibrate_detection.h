#pragma once

#include <Arduino.h>

// 振动触发事件，记录触发时三轴 ADC 值和本次冲击的峰值特征。
struct VibrateEvent {
    uint64_t timestampUs = 0;
    uint16_t rawX = 0;
    uint16_t rawY = 0;
    uint16_t rawZ = 0;
    float peakMagnitude = 0.0F;
    float peakJerk = 0.0F;
    bool saturated = false;
};

class VibrateDetection {
public:
    // 配置三轴 ADC 输入和传感器休眠控制脚。
    void begin(uint8_t xPin, uint8_t yPin, uint8_t zPin, uint8_t sleepPin);
    void update(uint64_t nowUs);
    bool consumeEvent(VibrateEvent& event);  // 取走单个待处理事件。

    bool flag() const { return eventPending_; }
    bool calibrated() const { return calibrated_; }
    uint16_t rawX() const { return rawX_; }
    uint16_t rawY() const { return rawY_; }
    uint16_t rawZ() const { return rawZ_; }
    float magnitude() const { return magnitude_; }
    float jerk() const { return jerk_; }
    uint32_t droppedSamples() const { return droppedSamples_; }

private:
    uint8_t xPin_ = 0;
    uint8_t yPin_ = 0;
    uint8_t zPin_ = 0;
    uint8_t sleepPin_ = 0;
    uint32_t sampleIntervalUs_ = 0;
    uint64_t nextSampleUs_ = 0;
    uint64_t lastTriggerUs_ = 0;
    uint32_t droppedSamples_ = 0;

    uint16_t rawX_ = 0;
    uint16_t rawY_ = 0;
    uint16_t rawZ_ = 0;
    float baselineX_ = 0.0F;
    float baselineY_ = 0.0F;
    float baselineZ_ = 0.0F;
    float previousMagnitude_ = 0.0F;
    float magnitude_ = 0.0F;
    float jerk_ = 0.0F;
    float calibrationSumX_ = 0.0F;
    float calibrationSumY_ = 0.0F;
    float calibrationSumZ_ = 0.0F;
    uint16_t calibrationCount_ = 0;
    uint8_t consecutiveTriggerSamples_ = 0;
    float candidatePeakMagnitude_ = 0.0F;
    float candidatePeakJerk_ = 0.0F;
    bool candidateSaturated_ = false;
    bool calibrated_ = false;
    bool eventPending_ = false;
    VibrateEvent pendingEvent_;
};
