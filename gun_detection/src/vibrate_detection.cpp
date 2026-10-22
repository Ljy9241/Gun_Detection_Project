#include "vibrate_detection.h"

#include <math.h>

#include "system_config.h"

void VibrateDetection::begin(uint8_t xPin, uint8_t yPin, uint8_t zPin, uint8_t sleepPin) {
    xPin_ = xPin;
    yPin_ = yPin;
    zPin_ = zPin;
    sleepPin_ = sleepPin;

    pinMode(sleepPin_, OUTPUT);
    digitalWrite(sleepPin_, HIGH);  // 拉高唤醒 MMA7361。
    pinMode(xPin_, INPUT);
    pinMode(yPin_, INPUT);
    pinMode(zPin_, INPUT);
    analogSetPinAttenuation(xPin_, ADC_11db);
    analogSetPinAttenuation(yPin_, ADC_11db);
    analogSetPinAttenuation(zPin_, ADC_11db);

    sampleIntervalUs_ = 1000000UL / config::VIBRATE_SAMPLE_RATE_HZ;
    nextSampleUs_ = micros() + 1000;  // MMA7361 enable response is sub-millisecond.
}

void VibrateDetection::update(uint64_t nowUs) {
    if (nowUs < nextSampleUs_) {
        return;
    }

    // 若循环严重延迟，跳过错过的采样点，不在同一时刻突发补采。
    if (nowUs - nextSampleUs_ > sampleIntervalUs_ * 4ULL) {
        droppedSamples_ += static_cast<uint32_t>((nowUs - nextSampleUs_) / sampleIntervalUs_);
        nextSampleUs_ = nowUs;
    }
    nextSampleUs_ += sampleIntervalUs_;

    rawX_ = analogRead(xPin_);
    rawY_ = analogRead(yPin_);
    rawZ_ = analogRead(zPin_);

    // 上电初期取静止样本均值作为重力与安装方向的基线。
    if (!calibrated_) {
        calibrationSumX_ += rawX_;
        calibrationSumY_ += rawY_;
        calibrationSumZ_ += rawZ_;
        ++calibrationCount_;
        if (calibrationCount_ >= config::VIBRATE_CALIBRATION_SAMPLES) {
            baselineX_ = calibrationSumX_ / calibrationCount_;
            baselineY_ = calibrationSumY_ / calibrationCount_;
            baselineZ_ = calibrationSumZ_ / calibrationCount_;
            calibrated_ = true;
        }
        return;
    }

    // 去除各轴静态基线后计算三维偏移幅值，并用相邻幅值差近似冲击变化率。
    const float dx = rawX_ - baselineX_;
    const float dy = rawY_ - baselineY_;
    const float dz = rawZ_ - baselineZ_;
    magnitude_ = sqrtf(dx * dx + dy * dy + dz * dz);
    jerk_ = fabsf(magnitude_ - previousMagnitude_);
    previousMagnitude_ = magnitude_;

    // 用幅值和尖锐变化共同筛选冲击，并要求连续样本确认以降低孤立噪声误报。
    const bool magnitudeHigh = magnitude_ >= config::VIBRATE_PEAK_THRESHOLD_COUNTS;
    const bool startsWithSharpEdge = jerk_ >= config::VIBRATE_JERK_THRESHOLD_COUNTS;
    const bool saturatedNow = rawX_ < 16 || rawX_ > 4079 || rawY_ < 16 || rawY_ > 4079 ||
                              rawZ_ < 16 || rawZ_ > 4079;
    if (magnitudeHigh && (consecutiveTriggerSamples_ > 0 || startsWithSharpEdge)) {
        candidatePeakMagnitude_ = max(candidatePeakMagnitude_, magnitude_);
        candidatePeakJerk_ = max(candidatePeakJerk_, jerk_);
        candidateSaturated_ = candidateSaturated_ || saturatedNow;
        ++consecutiveTriggerSamples_;
    } else {
        consecutiveTriggerSamples_ = 0;
        candidatePeakMagnitude_ = 0.0F;
        candidatePeakJerk_ = 0.0F;
        candidateSaturated_ = false;
    }

    const bool retriggerReady = lastTriggerUs_ == 0 ||
                                nowUs - lastTriggerUs_ >= config::VIBRATE_RETRIGGER_US;
    if (consecutiveTriggerSamples_ >= config::VIBRATE_TRIGGER_SAMPLES && retriggerReady) {
        pendingEvent_ = {nowUs, rawX_, rawY_, rawZ_, candidatePeakMagnitude_,
                         candidatePeakJerk_, candidateSaturated_};
        eventPending_ = true;
        lastTriggerUs_ = nowUs;
        consecutiveTriggerSamples_ = 0;
        candidatePeakMagnitude_ = 0.0F;
        candidatePeakJerk_ = 0.0F;
        candidateSaturated_ = false;
    }

    // Adapt only while quiet, so gravity/orientation changes during an impact do not erase it.
    if (magnitude_ < config::VIBRATE_QUIET_THRESHOLD_COUNTS) {
        constexpr float alpha = 0.002F;
        baselineX_ += alpha * (rawX_ - baselineX_);
        baselineY_ += alpha * (rawY_ - baselineY_);
        baselineZ_ += alpha * (rawZ_ - baselineZ_);
    }
}

bool VibrateDetection::consumeEvent(VibrateEvent& event) {
    if (!eventPending_) {
        return false;
    }
    event = pendingEvent_;
    eventPending_ = false;
    return true;
}
