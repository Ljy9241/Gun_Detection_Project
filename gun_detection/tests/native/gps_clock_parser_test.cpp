// Compile against the actual TinyGPSPlus library, not the event test's GPS stub.
#include <TinyGPS++.h>
#include "gps_clock_policy.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <iostream>

static void rmc(TinyGPSPlus& gps, const std::string& utc, const std::string& date) {
    const std::string body = "GPRMC," + utc + ",V,,,,,,," + date + ",,,N";
    uint8_t checksum = 0;
    for (const char c : body) checksum ^= static_cast<uint8_t>(c);
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "*%02X\r\n", checksum);
    for (const char c : "$" + body + suffix) gps.encode(c);
}

static bool candidate(TinyGPSPlus& gps, int64_t& epoch) {
    return gps_clock::utcEpoch(gps.date.year(), gps.date.month(), gps.date.day(),
                               gps.time.hour(), gps.time.minute(), gps.time.second(),
                               gps.time.centisecond(), epoch);
}

int main() {
    TinyGPSPlus gps;
    rmc(gps, "000010.00", "010100");
    assert(gps.date.isValid() && gps.time.isValid());
    assert(!gps.location.isValid());
    int64_t epoch = 0;
    // Reproduces the flaw: a checksum-valid RMC with V status can offer year 2000.
    assert(gps.date.year() == 2000);
    assert(!candidate(gps, epoch));

    int64_t rtc = 0;
    for (int window = 0; window < 3; ++window) {
        gps = TinyGPSPlus{};
        rmc(gps, "000000.00", "");  // Empty date still gets committed by the library.
        assert(gps.date.isValid() && gps.time.isValid());
        assert(!candidate(gps, epoch));
        assert(window == 0 || rtc >= gps_clock::MIN_VALID_EPOCH);
        rmc(gps, "112839.00", "081126");
        assert(candidate(gps, epoch));
        assert(gps_clock::maySync(epoch, rtc));
        rtc = epoch;
    }
    std::cout << "PASS: actual TinyGPSPlus invalid/empty startup dates rejected across repeated GPS resets\n";
}
