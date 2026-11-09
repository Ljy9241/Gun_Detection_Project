#include "standby_test.h"
#include <cassert>
#include <iostream>

int main() {
    constexpr uint64_t minute = 60000000ULL;
    StandbyTest test(2 * minute, 2 * minute);
    assert(!test.requested(1000000000));  // Never triggers before setup arms it.
    test.arm(500000);
    assert(!test.requested(500000 + 2 * minute - 1));
    assert(test.requested(500000 + 2 * minute));
    // A slow ADC stop / Flash flush must not consume any of the sleep duration.
    assert(test.requested(500000 + 3 * minute));
    test.beginSleep(500000 + 3 * minute);
    assert(test.remainingUs(500000 + 3 * minute) == 2 * minute);
    assert(test.remainingUs(500000 + 4 * minute) == minute);
    test.beginSleep(500000 + 4 * minute);  // Early wake/retry cannot extend sleep.
    assert(test.remainingUs(500000 + 4 * minute) == minute);
    assert(test.requested(500000 + 5 * minute - 1));
    assert(!test.requested(500000 + 5 * minute));
    assert(test.remainingUs(500000 + 5 * minute) == 0);
    test.finish();
    assert(!test.requested(500000 + 100 * minute));

    // Exercise timestamps beyond the 32-bit millis rollover.
    StandbyTest longUptime(minute, minute);
    const uint64_t start = (uint64_t{1} << 32) * 1000 - minute / 2;
    longUptime.arm(start);
    assert(!longUptime.requested(start + minute - 1));
    assert(longUptime.requested(start + minute));
    longUptime.beginSleep(start + minute);
    assert(longUptime.remainingUs(start + minute + 1) == minute - 1);
    assert(!longUptime.requested(start + 2 * minute));
    std::cout << "PASS: one-shot countdown, shutdown latency, early wake, exact end, no repeat, 64-bit uptime\n";
}
