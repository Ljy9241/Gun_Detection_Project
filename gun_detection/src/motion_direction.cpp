#include "motion_direction.h"
#include <cmath>

void MotionDirectionDetector::reset() {
    // 不清零整个缓冲，避免在采样任务栈上创建大临时对象。
    calibrationHead_ = calibrationCount_ = 0;
    hasInput_ = hasProcessed_ = calibrated_ = false;
    candidate_ = active_ = quiet_ = pending_ = false;
    cooldownUntilUs_ = 0;
}

bool MotionDirectionDetector::consumeEvent(MotionEvent& event) {
    if (!pending_) return false;
    event = pendingEvent_;
    pending_ = false;
    return true;
}

void MotionDirectionDetector::processSample(uint16_t x, uint16_t y, uint16_t z, uint64_t nowUs) {
    using namespace motion_config;
    if (hasInput_ && (nowUs <= lastInputUs_ || nowUs - lastInputUs_ > MAX_GAP_US)) reset();
    lastInputUs_ = nowUs;
    hasInput_ = true;
    // 1 kHz ADC 输入每 50 ms 取一个完整三轴样本，不对幅度求平均。
    if (hasProcessed_ && nowUs - lastProcessedUs_ < SAMPLE_INTERVAL_US) return;
    const float dt = hasProcessed_ ? (nowUs - lastProcessedUs_) / 1000000.0F : 0;
    lastProcessedUs_ = nowUs;
    hasProcessed_ = true;
    const float a[3] = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)};
    if (!calibrated_) {
        const size_t capacity = sizeof(calibration_) / sizeof(calibration_[0]);
        if (calibrationCount_ == capacity) {
            calibrationHead_ = (calibrationHead_ + 1) % capacity;
            --calibrationCount_;
        }
        auto& entry = calibration_[(calibrationHead_ + calibrationCount_++) % capacity];
        entry.time = nowUs;
        for (int j = 0; j < 3; ++j) entry.a[j] = a[j];
        while (calibrationCount_ > 1 &&
               nowUs - calibration_[(calibrationHead_ + 1) % capacity].time >= CALIBRATION_US) {
            calibrationHead_ = (calibrationHead_ + 1) % capacity;
            --calibrationCount_;
        }
        if (calibrationCount_ < 20 || nowUs - calibration_[calibrationHead_].time < CALIBRATION_US) return;
        float sums[3] = {}, lo[3] = {4095, 4095, 4095}, hi[3] = {};
        for (size_t i = 0; i < calibrationCount_; ++i) {
            const auto& sample = calibration_[(calibrationHead_ + i) % capacity];
            for (int j = 0; j < 3; ++j) {
                sums[j] += sample.a[j];
                if (sample.a[j] < lo[j]) lo[j] = sample.a[j];
                if (sample.a[j] > hi[j]) hi[j] = sample.a[j];
            }
        }
        for (int j = 0; j < 3; ++j) if (hi[j] - lo[j] >= 100) return;
        for (int j = 0; j < 3; ++j) baseline_[j] = sums[j] / calibrationCount_;
        calibrated_ = true;
        return;
    }
    float d[3];
    for (int j = 0; j < 3; ++j) d[j] = a[j] - baseline_[j];
    const float magnitude = std::sqrt(d[0]*d[0] + d[1]*d[1] + d[2]*d[2]);
    if (!active_) {
        if (nowUs < cooldownUntilUs_ || magnitude <= START) {
            candidate_ = false;
            const float alpha = 1.0F - std::exp(-dt);
            for (int j = 0; j < 3; ++j) baseline_[j] += alpha * d[j];
            return;
        }
        if (!candidate_ || nowUs - candidateUs_ > CANDIDATE_US) {
            candidate_ = true;
            candidateUs_ = nowUs;
            candidatePeak_ = magnitude;
            for (int j = 0; j < 3; ++j) {
                candidateDelta_[j] = d[j];
                candidateRaw_[j] = static_cast<uint16_t>(a[j]);
            }
            return;
        }
        current_ = MotionEvent{};
        current_.startUs = candidateUs_;
        current_.energyX = candidateDelta_[0]*candidateDelta_[0];
        current_.energyY = candidateDelta_[1]*candidateDelta_[1];
        current_.energyZ = candidateDelta_[2]*candidateDelta_[2];
        current_.peak = candidatePeak_;
        current_.peakX = candidateRaw_[0]; current_.peakY = candidateRaw_[1]; current_.peakZ = candidateRaw_[2];
        strongSamples_ = 1;
        quiet_ = false;
        active_ = true;
        candidate_ = false;
    }
    current_.energyX += d[0]*d[0];
    current_.energyY += d[1]*d[1];
    current_.energyZ += d[2]*d[2];
    if (magnitude > current_.peak) {
        current_.peak = magnitude;
        current_.peakX = x; current_.peakY = y; current_.peakZ = z;
    }
    if (magnitude > START) ++strongSamples_;
    if (magnitude < STOP) {
        if (!quiet_) { quiet_ = true; quietSinceUs_ = nowUs; }
    } else quiet_ = false;
    const bool timedOut = nowUs - current_.startUs >= MAX_DURATION_US;
    if (!timedOut && !(quiet_ && nowUs - quietSinceUs_ >= QUIET_US)) return;
    const float total = current_.energyX + current_.energyY + current_.energyZ;
    if (!timedOut && strongSamples_ >= 3 && current_.peak >= MIN_PEAK && total > 0) {
        if (current_.energyZ >= DOMINANCE * total && current_.energyZ >= AXIS_RATIO * current_.energyX)
            current_.direction = MotionDirection::Horizontal;
        else if (current_.energyX >= DOMINANCE * total && current_.energyX >= AXIS_RATIO * current_.energyZ)
            current_.direction = MotionDirection::Vertical;
    }
    current_.timestampUs = nowUs;
    // 不覆盖尚未消费的事件。
    if (!pending_) { pendingEvent_ = current_; pending_ = true; }
    active_ = false;
    cooldownUntilUs_ = nowUs + COOLDOWN_US;
}
