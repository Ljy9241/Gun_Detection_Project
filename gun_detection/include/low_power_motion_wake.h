#pragma once
#include "motion_direction.h"

// Raw motion can arrive before the first 20 Hz direction sample. Delay a
// normal-mode wake briefly, then wait for any in-progress classifier decision.
class LowPowerMotionWake {
public:
    void reset() { pending_ = false; }
    bool shouldExit(uint64_t now, bool moving, bool classifying, const MotionEvent* event) {
        if (moving && !pending_) { pending_ = true; since_ = now; }
        if (event) {
            const bool wake = pending_ && event->direction != MotionDirection::Vertical;
            reset();
            return wake;
        }
        if (pending_ && !classifying && now - since_ >=
            motion_config::CANDIDATE_US + motion_config::SAMPLE_INTERVAL_US) {
            reset();
            return true;
        }
        return false;
    }
private:
    bool pending_ = false;
    uint64_t since_ = 0;
};
