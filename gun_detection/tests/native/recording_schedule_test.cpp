#include "recording_schedule.h"
#include <cassert>
#include <iostream>

int main() {
    RecordingSchedule clock;
    unsigned sensors = 0, track = 0;
    for (uint32_t now = 0; now <= 20000; ++now) {
        const auto due = clock.poll(now, 10000, true, 1000);
        if (due.sensors) { assert(now == 10000 || now == 20000); ++sensors; }
        if (due.track) { assert(now % 1000 == 0); ++track; }
    }
    assert(sensors == 2 && track == 20);
    // Low-power mode stops the 1-second track without changing sensor timing.
    assert(!clock.poll(21000, 10000, false, 1000).track);
    assert(clock.poll(30000, 10000, false, 1000).sensors);
    assert(!clock.poll(30001, 10000, true, 1000).track);
    assert(clock.poll(31001, 10000, true, 1000).track);
    // Each mode can use a different sensor interval; extra track polls do not reset it.
    assert(clock.poll(35000, 5000, false, 1000).sensors);
    assert(!clock.poll(44999, 10000, false, 1000).sensors);
    assert(clock.poll(45000, 10000, false, 1000).sensors);
    // Resume waits for fresh samples and skips the sleep backlog.
    clock.reset(100000);
    auto due = clock.poll(100000, 10000, true, 1000);
    assert(!due.sensors && !due.track);
    assert(clock.poll(101000, 10000, true, 1000).track);
    assert(clock.poll(110000, 10000, true, 1000).sensors);
    // Millis wrap must not stall either independent clock.
    const uint32_t start = UINT32_MAX - 499;
    clock.reset(start);
    clock.poll(start, 10000, true, 1000);
    assert(clock.poll(start + uint32_t(1000), 10000, true, 1000).track);
    assert(clock.poll(start + uint32_t(10000), 10000, true, 1000).sensors);
    std::cout << "PASS: independent 1s GPS/10s sensors, low-power switch, configurable intervals, resume, rollover\n";
}
