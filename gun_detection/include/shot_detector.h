#pragma once

#include <Arduino.h>

#include "gps_manager.h"
#include "motion_direction.h"
#include "vibrate_detection.h"
#include "voice_detection.h"

// 横向动作对应的射击记录。
struct ShotEvent {
    uint32_t shotId = 0;
    uint32_t shotCount = 0;
    uint64_t timestampUs = 0;
    char timestamp[20] = {};  // RTC 时间，格式 YYYY-MM-DD HH:MM:SS；未校时则为空。
    VoiceEvent voice;
    VibrateEvent vibration;
    GpsFix gps;
    MotionEvent motion;
};

class ShotDetector {
public:
    // 仅横向动作增加射击计数。
    void onMotionEvent(const MotionEvent& event);
    bool consumeShot(ShotEvent& event, const GpsFix& gpsFix);

    uint32_t shotCount() const { return shotCount_; }
    void clearPending() { shotPending_ = false; }

private:
    ShotEvent pendingShot_;
    uint64_t lastShotUs_ = 0;
    uint32_t shotCount_ = 0;
    bool shotPending_ = false;
};
