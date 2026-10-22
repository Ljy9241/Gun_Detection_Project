#pragma once

#include <Arduino.h>

// 声音触发事件：包含峰峰值、均方根幅值和相对背景的突增比例。
struct VoiceEvent {
    uint64_t timestampUs = 0;
    uint16_t peakToPeak = 0;
    float rms = 0.0F;
    float riseRatio = 0.0F;
};

class VoiceDetection {
public:
    // 初始化麦克风 ADC 与固定采样节拍。
    void begin(uint8_t adcPin);
    void update(uint64_t nowUs);
    bool consumeEvent(VoiceEvent& event);

    bool flag() const { return eventPending_; }
    uint16_t latestRaw() const { return latestRaw_; }
    uint16_t latestPeakToPeak() const { return latestPeakToPeak_; }
    float latestRms() const { return latestRms_; }
    uint32_t droppedSamples() const { return droppedSamples_; }

private:
    // 清空当前采样帧累积值；每帧完成后重新开始统计。
    void resetFrame();
    // 根据当前帧的统计特征判断是否产生声音事件。
    void finishFrame(uint64_t nowUs);

    uint8_t adcPin_ = 0;
    uint32_t sampleIntervalUs_ = 0;
    uint64_t nextSampleUs_ = 0;
    uint64_t lastTriggerUs_ = 0;
    uint16_t sampleCount_ = 0;
    uint16_t minValue_ = 4095;
    uint16_t maxValue_ = 0;
    uint16_t latestRaw_ = 0;
    uint16_t latestPeakToPeak_ = 0;
    uint64_t sum_ = 0;
    uint64_t squareSum_ = 0;
    float latestRms_ = 0.0F;
    float previousRms_ = 1.0F;
    uint32_t droppedSamples_ = 0;
    bool eventPending_ = false;
    VoiceEvent pendingEvent_;
};
