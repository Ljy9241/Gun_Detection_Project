#include "daily_standby.h"
#include <cassert>
#include <iostream>

static int64_t beijing(unsigned day, unsigned hour, unsigned minute = 0, unsigned second = 0) {
    int64_t value = 0;
    assert(gps_clock::utcEpoch(2026, 11, day, hour, minute, second, 0, value));
    return value - 8 * 3600;
}

int main() {
    DailyStandby night(20 * 60, 8 * 60);
    assert(!night.contains(0));  // No GPS/NTP calendar yet: keep working.
    assert(!night.contains(gps_clock::MIN_VALID_EPOCH - 1));
    assert(!night.contains(beijing(9, 19, 59, 59)));
    assert(night.contains(beijing(9, 20)));
    assert(night.secondsUntilEnd(beijing(9, 20)) == 12 * 3600);
    assert(night.contains(beijing(9, 23, 59, 59)));
    assert(night.contains(beijing(10, 0)));
    assert(night.secondsUntilEnd(beijing(10, 0)) == 8 * 3600);
    assert(night.contains(beijing(10, 7, 59, 59)));
    assert(night.secondsUntilEnd(beijing(10, 7, 59, 59)) == 1);
    assert(!night.contains(beijing(10, 8)));
    assert(night.secondsUntilEnd(beijing(10, 8)) == 0);
    assert(!night.contains(beijing(10, 12)));
    assert(night.contains(beijing(10, 20)));  // Daily repetition.
    assert(night.secondsUntilEnd(beijing(10, 20, 15)) == 11 * 3600 + 45 * 60);

    DailyStandby daytime(9 * 60 + 30, 17 * 60 + 15);
    assert(!daytime.contains(beijing(9, 9, 29, 59)));
    assert(daytime.contains(beijing(9, 9, 30)));
    assert(daytime.secondsUntilEnd(beijing(9, 9, 30)) == 7 * 3600 + 45 * 60);
    assert(!daytime.contains(beijing(9, 17, 15)));
    assert(!DailyStandby(0, 0).contains(beijing(9, 20)));
    assert(!DailyStandby(1440, 480).contains(beijing(9, 20)));
    std::cout << "PASS: Beijing UTC conversion, overnight 20:00/08:00 boundaries, daily repeat, unknown time, configurable minutes\n";
}
