#pragma once
#include "telemetry_spool.h"
#include <cmath>

// Cloud firmware reuses the six ADC bytes without changing the Flash layout.
// reserved[0] = 1 distinguishes tilt snapshots from pre-upgrade ADC records.
inline void storeTelemetryTilt(TelemetryRecord& record, bool valid, float roll, float pitch) {
    record.reserved[0] = 1;
    valid = valid && std::isfinite(roll) && std::isfinite(pitch) &&
            roll >= -180 && roll <= 180 && pitch >= -90 && pitch <= 90;
    record.accelX = valid ? static_cast<uint16_t>(std::lround((roll + 180) * 100)) : 0;
    record.accelY = valid ? static_cast<uint16_t>(std::lround((pitch + 90) * 100)) : 0;
    record.accelZ = valid ? 1 : 0;
}
