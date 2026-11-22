#ifdef DEVICE_ARCHIVE_UPLOAD
#include "archive_time.h"
#include <esp_timer.h>
#include <sys/time.h>

namespace {
TelemetryTimeAnchor current;
uint64_t nextSaveMs = 0;
bool saved = false;
}

bool captureArchiveTime(uint64_t boot) {
    if (saved && current.boot == boot) return true;
    const uint64_t uptime = esp_timer_get_time() / 1000ULL;
    if (uptime < nextSaveMs) return false;
    timeval now{};
    gettimeofday(&now, nullptr);
    if (now.tv_sec < 1704067200) return false;
    if (current.boot != boot) {
        current = {};
        current.boot = boot;
        current.utcMs = uint64_t(now.tv_sec) * 1000 + now.tv_usec / 1000;
        current.uptimeMs = uptime;
        saved = false;
    }
    nextSaveMs = uptime + 1000;
    saved = saveTimeAnchor("/littlefs", current);
    return saved;
}

bool readArchiveTime(uint64_t boot, TelemetryTimeAnchor& anchor) {
    if (saved && current.boot == boot) {
        anchor = current;
        return true;
    }
    return loadTimeAnchor("/littlefs", boot, anchor);
}
#endif
