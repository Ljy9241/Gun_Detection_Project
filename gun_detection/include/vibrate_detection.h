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
    // 配置三轴输入脚和传感器休眠控制脚；ADC 样本由 AdcSampler 提供。
    void begin(uint8_t xPin, uint8_t yPin, uint8_t zPin, uint8_t sleepPin);
    void update(uint64_t nowUs);
    void processSample(uint16_t rawX, uint16_t rawY, uint16_t rawZ, uint64_t timestampUs);
    bool consumeEvent(VibrateEvent& event);  // 取走单个待处理事件。

    bool flag() const { return eventPending_; }
    bool calibrated() const { return calibrated_; }
    uint16_t rawX() const { return rawX_; }
    uint16_t rawY() const { return rawY_; }
    uint16_t rawZ() const { return rawZ_; }
    float magnitude() const { return magnitude_; }
    float jerk() const { return jerk_; }
    float sampleDelta() const { return sampleDelta_; }
    bool angleValid() const { return angleValid_; }
    float rollDegrees() const { return rollDegrees_; }
    float pitchDegrees() const { return pitchDegrees_; }
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
    uint16_t previousRawX_ = 0;
    uint16_t previousRawY_ = 0;
    uint16_t previousRawZ_ = 0;
    float sampleDelta_ = 0.0F;
    float baselineX_ = 0.0F;
    float baselineY_ = 0.0F;
    float baselineZ_ = 0.0F;
    float angleZeroX_ = 0.0F;
    float angleZeroY_ = 0.0F;
    float angleZeroZ_ = 0.0F;
    float countsPerG_ = 0.0F;
    float angleZSign_ = 1.0F;
    float angleCalibrationSumX_ = 0.0F;
    float angleCalibrationSumY_ = 0.0F;
    float angleCalibrationSumZ_ = 0.0F;
    uint16_t angleCalibrationCount_ = 0;
    float filteredAccelX_ = 0.0F;
    float filteredAccelY_ = 0.0F;
    float filteredAccelZ_ = 1.0F;
    float rollDegrees_ = 0.0F;
    float pitchDegrees_ = 0.0F;
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
    bool previousRawSampleValid_ = false;
    bool angleValid_ = false;
    bool angleCalibrationComplete_ = false;
    bool angleFilterInitialized_ = false;
    bool eventPending_ = false;
    VibrateEvent pendingEvent_;
};
