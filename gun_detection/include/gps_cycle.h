#pragma once
#include <cstdint>

// Start-to-start schedule independent of motion state; unsigned elapsed time
// handles millis() wrap. Missed windows are skipped rather than replayed.
class GpsCycle {
public:
    GpsCycle(uint32_t periodMs, uint32_t windowMs)
        : periodMs_(periodMs), windowMs_(windowMs) {}
    bool startIfDue(uint32_t now) {
        if (active_ || (started_ && now - startMs_ < periodMs_)) return false;
        startNow(now);
        return true;
    }
    // A transition between continuous and periodic operation begins a fresh
    // window, so a long continuous session cannot cause an immediate timeout.
    void startNow(uint32_t now) {
        started_ = active_ = true;
        startMs_ = now;
    }
    bool active() const { return active_; }
    bool timedOut(uint32_t now) const {
        return active_ && now - startMs_ >= windowMs_;
    }
    void finish() { active_ = false; }
private:
    uint32_t periodMs_, windowMs_, startMs_ = 0;
    bool started_ = false, active_ = false;
};
