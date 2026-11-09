#include "gps_cycle.h"
#include <cassert>
#include <iostream>

int main() {
    GpsCycle cycle(600000, 180000);
    assert(cycle.startIfDue(100));
    assert(cycle.active());
    assert(!cycle.startIfDue(200));
    assert(!cycle.timedOut(180099));
    assert(cycle.timedOut(180100));
    cycle.finish();
    assert(!cycle.active());
    assert(!cycle.startIfDue(600099));
    assert(cycle.startIfDue(600100));
    // A successful fix ends the window early without shifting the next start.
    cycle.finish();
    assert(!cycle.startIfDue(600101));
    assert(cycle.startIfDue(1200100));
    // Upload-only cancellation preserves the last start, and skips missed slots.
    cycle.finish();
    assert(cycle.startIfDue(5000000));
    cycle.finish();
    assert(!cycle.startIfDue(5000001));
    assert(cycle.startIfDue(5600000));

    GpsCycle wrap(600000, 180000);
    const uint32_t start = UINT32_MAX - 999;
    assert(wrap.startIfDue(start));
    assert(!wrap.timedOut(start + uint32_t(179999)));
    assert(wrap.timedOut(start + uint32_t(180000)));
    wrap.finish();
    assert(!wrap.startIfDue(start + uint32_t(599999)));
    assert(wrap.startIfDue(start + uint32_t(600000)));
    std::cout << "PASS: GPS first start, timeout, early finish, suspension, skipped slots, rollover\n";
}
