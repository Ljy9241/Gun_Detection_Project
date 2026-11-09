#pragma once
#include <cstdint>
#include "gps_clock_policy.h"

// UTC system clock, fixed Beijing offset. Interval is [start, end).
class DailyStandby {
public:
    DailyStandby(unsigned startMinute, unsigned endMinute)
        : start_(startMinute), end_(endMinute) {}
    bool contains(int64_t utc) const {
        if (utc < gps_clock::MIN_VALID_EPOCH || start_ >= 1440 || end_ >= 1440 || start_ == end_) return false;
        const unsigned minute = localSecond(utc) / 60;
        return start_ < end_ ? minute >= start_ && minute < end_
                            : minute >= start_ || minute < end_;
    }
    uint32_t secondsUntilEnd(int64_t utc) const {
        if (!contains(utc)) return 0;
        return (end_ * 60 + 86400 - localSecond(utc)) % 86400;
    }
private:
    static unsigned localSecond(int64_t utc) {
        return static_cast<unsigned>((utc + 8 * 3600) % 86400);
    }
    unsigned start_, end_;
};
