#pragma once
#include "telemetry_time.h"

// Only called by the Flash owner; capture even when Wi-Fi is off.
bool captureArchiveTime(uint64_t boot);
bool readArchiveTime(uint64_t boot, TelemetryTimeAnchor& anchor);
