#include "telemetry_tilt.h"
#include <cassert>
#include <limits>
int main() {
    TelemetryRecord r;
    assert(r.reserved[0] == 0);
    storeTelemetryTilt(r, true, -123.45F, 67.89F);
    assert(r.reserved[0] == 1 && r.accelZ == 1);
    assert(r.accelX == 5655 && r.accelY == 15789);
    storeTelemetryTilt(r, true, -180, -90);
    assert(r.accelX == 0 && r.accelY == 0 && r.accelZ == 1);
    storeTelemetryTilt(r, true, 180, 90);
    assert(r.accelX == 36000 && r.accelY == 18000);
    storeTelemetryTilt(r, false, 0, 0);
    assert(r.accelZ == 0);
    storeTelemetryTilt(r, true, std::numeric_limits<float>::quiet_NaN(), 0);
    assert(r.accelZ == 0);
    storeTelemetryTilt(r, true, 181, 0);
    assert(r.accelZ == 0);
}
