#pragma once
#include <cstdint>

namespace gps_clock {
// Match the application's earliest usable calendar date (2024-01-01 UTC).
constexpr int64_t MIN_VALID_EPOCH = 1704067200;
constexpr int64_t MAX_CORRECTION_SECONDS = 300;

// TinyGPSPlus date/time validity means a field was committed after a checksum,
// including RMC sentences with status V. Validate the values before RTC writes.
inline bool utcEpoch(int year, unsigned month, unsigned day, unsigned hour,
                     unsigned minute, unsigned second, unsigned centisecond,
                     int64_t& epoch) {
    if (year < 2024 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || hour > 23 || minute > 59 || second > 59 || centisecond > 99) return false;
    const unsigned monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > monthDays[month - 1] + (month == 2 && leap ? 1U : 0U)) return false;
    year -= month <= 2;
    const int era = year / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
    const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
    epoch = days * 86400 + hour * 3600 + minute * 60 + second;
    return epoch >= MIN_VALID_EPOCH;
}

inline bool maySync(int64_t candidate, int64_t current) {
    if (candidate < MIN_VALID_EPOCH) return false;
    if (current < MIN_VALID_EPOCH) return true;  // First calendar synchronization.
    // Keep the running RTC when receiver startup data proposes a large jump.
    const int64_t delta = candidate - current;
    return delta >= -MAX_CORRECTION_SECONDS && delta <= MAX_CORRECTION_SECONDS;
}
}  // namespace gps_clock
