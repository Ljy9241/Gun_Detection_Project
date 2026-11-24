#pragma once
#include <cstdint>
#include <cstddef>
#include "motion_config.h"

enum class MotionDirection { Unknown, Horizontal, Vertical };
struct MotionEvent {
    MotionDirection direction = MotionDirection::Unknown;
    uint64_t startUs = 0, timestampUs = 0;  // 结束确认时刻。
    float energyX = 0, energyY = 0, energyZ = 0, peak = 0;
    uint16_t peakX = 0, peakY = 0, peakZ = 0;
};

// 由采样任务调用，跨任务读取/复位需使用同一把锁。无动态内存分配。
class MotionDirectionDetector {
public:
    void reset();
    void processSample(uint16_t x, uint16_t y, uint16_t z, uint64_t nowUs);
    bool consumeEvent(MotionEvent& event);
    bool calibrated() const { return calibrated_; }
    bool classifying() const { return candidate_ || active_; }
private:
    struct CalibrationSample { uint64_t time; float a[3]; };
    CalibrationSample calibration_[motion_config::CALIBRATION_US / motion_config::SAMPLE_INTERVAL_US + 2] = {};
    size_t calibrationHead_ = 0, calibrationCount_ = 0;
    float baseline_[3] = {}, candidateDelta_[3] = {};
    uint16_t candidateRaw_[3] = {};
    float candidatePeak_ = 0;
    uint64_t lastInputUs_ = 0, lastProcessedUs_ = 0;
    uint64_t candidateUs_ = 0, quietSinceUs_ = 0, cooldownUntilUs_ = 0;
    uint16_t strongSamples_ = 0;
    bool hasInput_ = false, hasProcessed_ = false, calibrated_ = false;
    bool candidate_ = false, active_ = false, quiet_ = false, pending_ = false;
    MotionEvent current_, pendingEvent_;
};
