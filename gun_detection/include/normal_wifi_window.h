#pragma once
#include <cstdint>
#include "motion_direction.h"

// 按首次成功关联热点的时刻计时；掉线或重新发现上位机都不延长窗口。
class NormalWifiWindow {
public:
    bool request(MotionDirection direction, bool alwaysOn, bool lowPower, bool uploadOnly, uint32_t now) {
        if (alwaysOn || lowPower || uploadOnly || direction != MotionDirection::Vertical) return false;
        state_ = Waiting;
        since_ = now;
        return true;
    }
    void cancel() { state_ = Idle; }
    bool active() const { return state_ != Idle; }
    bool update(uint32_t now, bool connected, uint32_t hold, uint32_t timeout) {
        if (!active()) return false;
        if (state_ == Waiting && connected) {
            state_ = Holding;
            since_ = now;
        }
        if (now - since_ >= (state_ == Holding ? hold : timeout)) cancel();
        return active();
    }
private:
    enum { Idle, Waiting, Holding } state_ = Idle;
    uint32_t since_ = 0;
};
