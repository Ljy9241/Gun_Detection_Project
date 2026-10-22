#include "voice_detection.h"

#include <math.h>

#include "system_config.h"

void VoiceDetection::begin(uint8_t adcPin) {
    adcPin_ = adcPin;
    pinMode(adcPin_, INPUT);
    analogSetPinAttenuation(adcPin_, ADC_11db);
    sampleIntervalUs_ = 1000000UL / config::VOICE_SAMPLE_RATE_HZ;
    nextSampleUs_ = micros();
    resetFrame();
}

void VoiceDetection::resetFrame() {
    // 帧统计只保留当前窗口的极值、和与平方和。
    sampleCount_ = 0;
    minValue_ = 4095;
    maxValue_ = 0;
    sum_ = 0;
    squareSum_ = 0;
}

void VoiceDetection::update(uint64_t nowUs) {
    if (nowUs < nextSampleUs_) {
        return;
    }

    // Do not perform a burst of stale conversions after another subsystem delayed loop().
    if (nowUs - nextSampleUs_ > sampleIntervalUs_ * 4ULL) {
        droppedSamples_ += static_cast<uint32_t>((nowUs - nextSampleUs_) / sampleIntervalUs_);
        nextSampleUs_ = nowUs;
    }
    nextSampleUs_ += sampleIntervalUs_;

    // 每次调用至多进行一次 ADC 采样，保证实际采样间隔可控。
    const uint16_t raw = analogRead(adcPin_);
    latestRaw_ = raw;
    minValue_ = min(minValue_, raw);
    maxValue_ = max(maxValue_, raw);
    sum_ += raw;
    squareSum_ += static_cast<uint32_t>(raw) * raw;
    ++sampleCount_;

    if (sampleCount_ >= config::VOICE_FRAME_SAMPLES) {
        finishFrame(nowUs);
        resetFrame();
    }
}

void VoiceDetection::finishFrame(uint64_t nowUs) {
    // 由平方均值减均值平方求方差，RMS 表示交流分量强度。
    const float mean = static_cast<float>(sum_) / sampleCount_;
    float variance = static_cast<float>(squareSum_) / sampleCount_ - mean * mean;
    if (variance < 0.0F) {
        variance = 0.0F;
    }

    latestPeakToPeak_ = maxValue_ - minValue_;
    latestRms_ = sqrtf(variance);
    const float riseRatio = latestRms_ / max(previousRms_, 1.0F);

    // 同时要求声音幅度足够大、相对背景突然增强，并满足重触发间隔。
    const bool levelHigh = latestPeakToPeak_ >= config::VOICE_P2P_THRESHOLD &&
                           latestRms_ >= config::VOICE_RMS_THRESHOLD;
    const bool impulsive = riseRatio >= config::VOICE_RISE_RATIO;
    const bool retriggerReady = lastTriggerUs_ == 0 ||
                                nowUs - lastTriggerUs_ >= config::VOICE_RETRIGGER_US;

    if (levelHigh && impulsive && retriggerReady) {
        pendingEvent_ = {nowUs, latestPeakToPeak_, latestRms_, riseRatio};
        eventPending_ = true;
        lastTriggerUs_ = nowUs;
    }

    // A slow reference follows ordinary sound levels but does not immediately follow an impulse.
    if (!levelHigh) {
        previousRms_ = 0.85F * previousRms_ + 0.15F * max(latestRms_, 1.0F);
    }
}

bool VoiceDetection::consumeEvent(VoiceEvent& event) {
    if (!eventPending_) {
        return false;
    }
    event = pendingEvent_;
    eventPending_ = false;
    return true;
}
