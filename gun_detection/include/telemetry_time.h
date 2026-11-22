#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cstddef>

// Immutable, per-boot anchor. No dependence on upload/arrival time.
struct TelemetryTimeAnchor {
    uint64_t boot = 0;
    uint64_t utcMs = 0;
    uint64_t uptimeMs = 0;
    uint32_t checksum = 0;
};

inline uint32_t timeAnchorChecksum(const TelemetryTimeAnchor& a) {
    uint32_t sum = 2166136261u;
    const auto* bytes = reinterpret_cast<const uint8_t*>(&a);
    for (size_t i = 0; i < offsetof(TelemetryTimeAnchor, checksum); ++i)
        sum = (sum ^ bytes[i]) * 16777619u;
    return sum;
}

inline bool validTimeAnchor(const TelemetryTimeAnchor& a, uint64_t boot) {
    return boot && a.boot == boot && a.utcMs >= 1704067200000ULL &&
           a.utcMs <= 4102444799999ULL && a.uptimeMs <= a.utcMs &&
           a.checksum == timeAnchorChecksum(a);
}

inline bool inferTelemetryTime(const TelemetryTimeAnchor& a, uint64_t boot,
                               uint64_t uptimeMs, char* out, size_t size) {
    if (!validTimeAnchor(a, boot) || uptimeMs > a.uptimeMs) return false;
    const uint64_t utcMs = a.utcMs - (a.uptimeMs - uptimeMs);
    if (utcMs < 1704067200000ULL) return false;
    const time_t local = static_cast<time_t>(utcMs / 1000 + 8 * 3600);
    tm* parts = gmtime(&local); // Called only by the Flash owner, never the worker.
    return parts && strftime(out, size, "%Y-%m-%d %H:%M:%S", parts) == 19;
}

// Close + verify + atomic rename before any upload may use an anchor.
// Files share the record filesystem and survive application-only upgrades.
inline bool loadTimeAnchor(const char* directory, uint64_t boot, TelemetryTimeAnchor& a) {
    char path[160];
    snprintf(path, sizeof path, "%s/clock-%016llx.dat", directory, (unsigned long long)boot);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    const bool read = fread(&a, sizeof a, 1, f) == 1 && fgetc(f) == EOF;
    fclose(f);
    return read && validTimeAnchor(a, boot);
}

inline bool saveTimeAnchor(const char* directory, TelemetryTimeAnchor& a) {
    TelemetryTimeAnchor existing;
    if (loadTimeAnchor(directory, a.boot, existing)) {
        a = existing;
        return true;
    }
    a.checksum = timeAnchorChecksum(a);
    if (!validTimeAnchor(a, a.boot)) return false;
    char path[160], temp[164];
    snprintf(path, sizeof path, "%s/clock-%016llx.dat", directory, (unsigned long long)a.boot);
    snprintf(temp, sizeof temp, "%s.tmp", path);
    FILE* f = fopen(temp, "wb");
    if (!f) return false;
    const bool written = fwrite(&a, sizeof a, 1, f) == 1;
    const bool closed = fclose(f) == 0;
    if (!written || !closed || rename(temp, path) != 0) return false;
    TelemetryTimeAnchor check;
    return loadTimeAnchor(directory, a.boot, check) &&
           check.utcMs == a.utcMs && check.uptimeMs == a.uptimeMs;
}
