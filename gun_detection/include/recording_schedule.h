#pragma once
#include <cstdint>

// Independent clocks: GPS recording must never shift sensor recording times.
class RecordingSchedule {
public:
    struct Due { bool sensors = false, track = false; };
    Due poll(uint32_t now, uint32_t sensorInterval, bool trackEnabled, uint32_t trackInterval) {
        Due due;
        if (now - lastSensorMs_ >= sensorInterval) {
            due.sensors = true;
            lastSensorMs_ = now;
        }
        if (!trackEnabled || !trackEnabled_) {
            lastTrackMs_ = now;
        } else if (now - lastTrackMs_ >= trackInterval) {
            due.track = true;
            lastTrackMs_ = now;
        }
        trackEnabled_ = trackEnabled;
        return due;
    }
    void reset(uint32_t now) {
        lastSensorMs_ = lastTrackMs_ = now;
        trackEnabled_ = false;
    }
private:
    uint32_t lastSensorMs_ = 0, lastTrackMs_ = 0;
    bool trackEnabled_ = false;
};
