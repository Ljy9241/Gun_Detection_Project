#include "telemetry_time.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>

int main(int argc, char** argv) {
    assert(argc == 2); // Isolated scratch directory supplied by the test runner.
    std::filesystem::create_directories(argv[1]);
    TelemetryTimeAnchor a{};
    a.boot = 123;
    a.utcMs = 1794673890000ULL; // 2026-11-15 00:31:30 Beijing
    a.uptimeMs = 210000;
    a.checksum = timeAnchorChecksum(a);
    char stamp[20];
    assert(inferTelemetryTime(a, 123, 120000, stamp, sizeof stamp));
    assert(std::string(stamp) == "2026-11-15 00:30:00");
    assert(!inferTelemetryTime(a, 124, 120000, stamp, sizeof stamp));
    assert(!inferTelemetryTime(a, 123, 210001, stamp, sizeof stamp));
    assert(!inferTelemetryTime(a, 123, UINT64_MAX, stamp, sizeof stamp));
    assert(saveTimeAnchor(argv[1], a));
    TelemetryTimeAnchor loaded;
    assert(loadTimeAnchor(argv[1], 123, loaded));
    assert(inferTelemetryTime(loaded, 123, 120000, stamp, sizeof stamp));
    assert(std::string(stamp) == "2026-11-15 00:30:00");
    // Subsequent synchronization and reboot must reuse the persisted anchor.
    auto later = a;
    later.utcMs += 9000;
    assert(saveTimeAnchor(argv[1], later));
    assert(later.utcMs == a.utcMs);
    assert(!loadTimeAnchor(argv[1], 124, loaded));
    assert(!saveTimeAnchor("/nonexistent-parent-for-time-test/subdirectory", a));
    loaded = a;
    loaded.utcMs++;
    assert(!validTimeAnchor(loaded, 123));
    std::ofstream(std::string(argv[1]) + "/clock-000000000000007b.dat", std::ios::binary) << "broken";
    assert(!loadTimeAnchor(argv[1], 123, loaded));
}
