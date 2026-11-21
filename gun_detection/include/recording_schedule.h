#pragma once
#include <cstdint>

// Independent clocks: GPS recording must never shift sensor recording times.
class RecordingSchedule {
public:
    struct Due {
        bool sensors = false, track = false;
        uint32_t sensorLateMs = 0, sensorSkipped = 0;
    };
    Due poll(uint32_t now, uint32_t sensorInterval, bool trackEnabled, uint32_t trackInterval) {
        Due due;
        const uint32_t sensorElapsed = now - lastSensorMs_;
        if (sensorInterval && sensorElapsed >= sensorInterval) {
            due.sensors = true;
            const uint32_t periods = sensorElapsed / sensorInterval;
            lastSensorMs_ += periods * sensorInterval;
            due.sensorLateMs = sensorElapsed - periods * sensorInterval;
            due.sensorSkipped = periods - 1;
        }
        if (!trackEnabled || !trackEnabled_) {
            lastTrackMs_ = now;
        } else if (trackInterval && now - lastTrackMs_ >= trackInterval) {
            due.track = true;
            const uint32_t trackElapsed = now - lastTrackMs_;
            lastTrackMs_ += (trackElapsed / trackInterval) * trackInterval;
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
