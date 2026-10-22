#pragma once

#include <Arduino.h>

#include "gps_manager.h"
#include "vibrate_detection.h"
#include "voice_detection.h"

// 声音、振动与定位信息融合后的单次射击事件。
struct ShotEvent {
    uint32_t shotId = 0;
    uint32_t shotCount = 0;
    uint64_t timestampUs = 0;
    VoiceEvent voice;
    VibrateEvent vibration;
    GpsFix gps;
};

class ShotDetector {
public:
    // 两类传感器事件分别到达；任一到达后都会尝试进行时间窗融合。
    void onVoiceEvent(const VoiceEvent& event);
    void onVibrateEvent(const VibrateEvent& event);
    void update(uint64_t nowUs);
    bool consumeShot(ShotEvent& event, const GpsFix& gpsFix);

    bool flag1() const { return voiceValid_; }
    bool flag2() const { return vibrationValid_; }
    uint32_t shotCount() const { return shotCount_; }

private:
    // 仅当两种事件时间接近且冷却时间已过时，生成待消费事件。
    void tryFuse(uint64_t nowUs);

    VoiceEvent voiceEvent_;
    VibrateEvent vibrationEvent_;
    ShotEvent pendingShot_;
    uint64_t lastShotUs_ = 0;
    uint32_t shotCount_ = 0;
    bool voiceValid_ = false;
    bool vibrationValid_ = false;
    bool shotPending_ = false;  // 主循环尚未取走已融合事件。
};
