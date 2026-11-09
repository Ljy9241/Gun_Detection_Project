#include "gps_clock_policy.h"
#include <cassert>
#include <iostream>

int main() {
    int64_t epoch = 0;
    assert(gps_clock::utcEpoch(2024, 1, 1, 0, 0, 0, 0, epoch));
    assert(epoch == gps_clock::MIN_VALID_EPOCH);
    assert(gps_clock::utcEpoch(2026, 11, 8, 11, 28, 39, 25, epoch));
    const int64_t current = epoch;
    assert(gps_clock::maySync(epoch, 0));
    assert(gps_clock::maySync(epoch, current + 2));
    assert(gps_clock::maySync(epoch, current - 2));
    assert(!gps_clock::maySync(946684800, current));  // Startup 2000-01-01.
    assert(!gps_clock::maySync(0, current));
    assert(!gps_clock::maySync(current - 86400, current));
    assert(!gps_clock::maySync(current + 86400, current));
    assert(!gps_clock::utcEpoch(2000, 1, 1, 0, 0, 0, 0, epoch));
    assert(!gps_clock::utcEpoch(2026, 0, 0, 0, 0, 0, 0, epoch));
    assert(!gps_clock::utcEpoch(2026, 2, 29, 0, 0, 0, 0, epoch));
    assert(!gps_clock::utcEpoch(2026, 4, 31, 0, 0, 0, 0, epoch));
    assert(!gps_clock::utcEpoch(2026, 11, 8, 24, 0, 0, 0, epoch));
    assert(!gps_clock::utcEpoch(2026, 11, 8, 0, 60, 0, 0, epoch));
    assert(!gps_clock::utcEpoch(2026, 11, 8, 0, 0, 60, 0, epoch));
    assert(gps_clock::utcEpoch(2024, 2, 29, 23, 59, 59, 99, epoch));
    // Consecutive dates must stay continuous at leap-day and year boundaries.
    int64_t next = 0;
    assert(gps_clock::utcEpoch(2024, 3, 1, 0, 0, 0, 0, next));
    assert(next == epoch + 1);
    assert(gps_clock::utcEpoch(2026, 12, 31, 23, 59, 59, 0, epoch));
    assert(gps_clock::utcEpoch(2027, 1, 1, 0, 0, 0, 0, next));
    assert(next == epoch + 1);
    std::cout << "PASS: invalid GPS startup dates, RTC jump rejection, first sync, corrections, calendar boundaries\n";
}
