#include "shot_detector.h"

void ShotDetector::onMotionEvent(const MotionEvent& event) {
    if (event.direction != MotionDirection::Horizontal || shotPending_) return;
    if (shotCount_ != 0 && event.timestampUs <= lastShotUs_) return;
    pendingShot_ = ShotEvent{};
    pendingShot_.shotId = ++shotCount_;
    pendingShot_.shotCount = shotCount_;
    pendingShot_.timestampUs = event.timestampUs;
    pendingShot_.motion = event;
    // 兼容原 UDP 字段，填入横向动作峰值时的真实三轴读数。
    // 声音和 jerk 保持 0，不伪造声音融合或冲击测量。
    pendingShot_.vibration.timestampUs = event.timestampUs;
    pendingShot_.vibration.rawX = event.peakX;
    pendingShot_.vibration.rawY = event.peakY;
    pendingShot_.vibration.rawZ = event.peakZ;
    pendingShot_.vibration.peakMagnitude = event.peak;
    shotPending_ = true;
    lastShotUs_ = pendingShot_.timestampUs;
}

bool ShotDetector::consumeShot(ShotEvent& event, const GpsFix& gpsFix) {
    if (!shotPending_) {
        return false;
    }
    // 定位在主循环取事件时附加，使事件尽可能携带最新定位快照。
    pendingShot_.gps = gpsFix;
    event = pendingShot_;
    shotPending_ = false;
    return true;
}
