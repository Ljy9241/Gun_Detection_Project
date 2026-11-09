#pragma once
#include <cstdint>

// Monotonic one-shot test, independent of GPS/NTP and the daily schedule.
// Start the sleep duration only after ADC shutdown and Flash flush finish.
class StandbyTest {
public:
    StandbyTest(uint64_t delayUs, uint64_t sleepUs) : delayUs_(delayUs), sleepUs_(sleepUs) {}
    void arm(uint64_t nowUs) { armed_ = true; armedUs_ = nowUs; }
    bool requested(uint64_t nowUs) const {
        return armed_ && !completed_ && nowUs - armedUs_ >= delayUs_ &&
               (!sleepStarted_ || remainingUs(nowUs) > 0);
    }
    void beginSleep(uint64_t nowUs) {
        if (!sleepStarted_) { sleepStarted_ = true; sleepStartUs_ = nowUs; }
    }
    uint64_t remainingUs(uint64_t nowUs) const {
        if (!sleepStarted_) return sleepUs_;
        const uint64_t elapsed = nowUs - sleepStartUs_;
        return elapsed >= sleepUs_ ? 0 : sleepUs_ - elapsed;
    }
    void finish() { completed_ = true; }
private:
    uint64_t delayUs_, sleepUs_, armedUs_ = 0, sleepStartUs_ = 0;
    bool armed_ = false, sleepStarted_ = false, completed_ = false;
};
