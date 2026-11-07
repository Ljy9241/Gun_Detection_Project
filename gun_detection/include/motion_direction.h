#pragma once
#include <cstdint>
#include <cstddef>

enum class MotionDirection { Unknown, Horizontal, Vertical };
struct MotionEvent {
    MotionDirection direction = MotionDirection::Unknown;
    uint64_t startUs = 0, timestampUs = 0;  // 结束确认时刻。
    float energyX = 0, energyY = 0, energyZ = 0, peak = 0;
    uint16_t peakX = 0, peakY = 0, peakZ = 0;
};

// ADC counts；当前安装方向：横向 Z 主导，竖向 X 主导。
namespace motion_config {
constexpr uint64_t SAMPLE_INTERVAL_US = 50000;
constexpr uint64_t CALIBRATION_US = 2000000;
constexpr uint64_t MAX_GAP_US = 500000;
constexpr uint64_t CANDIDATE_US = 200000;
constexpr uint64_t QUIET_US = 500000;
constexpr uint64_t MAX_DURATION_US = 3000000;
constexpr uint64_t COOLDOWN_US = 1000000;
constexpr float START = 100.0F, STOP = 60.0F, MIN_PEAK = 200.0F;
constexpr float DOMINANCE = 0.65F, AXIS_RATIO = 3.0F;
}

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
    CalibrationSample calibration_[48] = {};
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
