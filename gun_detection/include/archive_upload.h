#pragma once
#include "telemetry_spool.h"

// Called only by the Flash queue's owner. The network task works on a copy.
void updateArchiveUpload(TelemetrySpool& spool);
void testArchiveReplayOnce();
