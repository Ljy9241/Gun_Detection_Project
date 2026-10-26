#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "shot_detector.h"
#include "system_config.h"
#include "telemetry_spool.h"

// 定时状态包所需的传感器快照和运行统计。
struct WifiTelemetry {
    uint16_t micRaw = 0;
    uint16_t micPeakToPeak = 0;
    float micRms = 0.0F;
    bool vibrationReady = false;
    uint16_t vibrationX = 0;
    uint16_t vibrationY = 0;
    uint16_t vibrationZ = 0;
    float vibrationMagnitude = 0.0F;
    float vibrationJerk = 0.0F;
    bool tiltValid = false;
    float rollDegrees = 0.0F;
    float pitchDegrees = 0.0F;
    bool voiceFlag = false;
    bool vibrationFlag = false;
    bool gpsValid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    double altitudeM = 0.0;
    double speedKmph = 0.0;
    double hdop = 0.0;
    uint32_t satellites = 0;
    uint32_t gpsAgeMs = UINT32_MAX;
    uint32_t gpsCharacters = 0;
    uint32_t gpsFailedChecksums = 0;
    uint32_t shotCount = 0;
    uint32_t voiceDetectionCount = 0;
    uint32_t vibrationDetectionCount = 0;
    uint32_t voiceDroppedSamples = 0;
    uint32_t vibrationDroppedSamples = 0;
};

class WifiTransport {
public:
    // 建立 Wi-Fi 站点连接；网络不可用时由 update 周期重试。
    void begin();
    void setTelemetry(const WifiTelemetry& telemetry);
    void setLowPowerMode(bool enabled, uint32_t nowMs);
    void update(uint32_t nowMs);
    bool enqueue(const ShotEvent& event);
    bool enqueueVoiceDetection(const VoiceEvent& event, uint32_t detectionCount);
    bool enqueueVibrationDetection(const VibrateEvent& event, uint32_t detectionCount);

    bool enabled() const { return enabled_; }
    bool radioEnabled() const { return radioEnabled_; }
    bool connected() const { return WiFi.status() == WL_CONNECTED; }
    uint8_t queuedEvents() const { return count_; }
    uint32_t sentEvents() const { return sentEvents_; }
    uint32_t sentStatuses() const { return sentStatuses_; }
    uint32_t telemetryWritesPerSecond() const { return telemetryWritesPerSecond_; }
    uint32_t telemetryWritesTotal() const { return telemetryWritesTotal_; }
    uint32_t failedSends() const { return failedSends_; }
    uint32_t droppedEvents() const { return droppedEvents_; }

private:
    static constexpr uint8_t QUEUE_SIZE = 16;
    static constexpr uint8_t TELEMETRY_BATCH_SIZE = 6;
    enum class EventType : uint8_t { Shot, Voice, Vibration };
    struct QueuedEvent {
        EventType type = EventType::Shot;
        ShotEvent shot;
        VoiceEvent voice;
        VibrateEvent vibration;
        uint32_t detectionCount = 0;
        char timestamp[20] = {};
    };
    bool enqueueEvent(const QueuedEvent& event);
    bool send(const QueuedEvent& event);
    // 上报周期状态包及最近一个统计窗口的传感器峰值。
    bool sendStatus(uint32_t nowMs);
    bool sendTelemetryBatch(const TelemetryRecord* records, size_t count);
    void receiveAcknowledgements();

    WiFiUDP udp_;
    TelemetrySpool spool_;
    QueuedEvent queue_[QUEUE_SIZE];
    uint8_t head_ = 0;  // 下一个写入位置。
    uint8_t tail_ = 0;
    uint8_t count_ = 0;
    uint32_t lastConnectAttemptMs_ = 0;
    uint32_t lastStatusMs_ = 0;
    uint32_t lastEventSendMs_ = 0;
    uint32_t lastSpoolMs_ = 0;
    uint32_t lastSpoolRateMs_ = 0;
    uint32_t lastSpoolRateTotal_ = 0;
    uint32_t lowPowerCycleStartMs_ = 0;
    uint32_t lastRecordSendMs_ = 0;
    uint64_t inFlightSequences_[TELEMETRY_BATCH_SIZE] = {};
    TelemetryRecord pendingFlashRecords_[config::TELEMETRY_FLASH_BATCH_RECORDS] = {};
    uint8_t inFlightCount_ = 0;
    uint8_t pendingFlashRecordCount_ = 0;
    uint32_t lastSendErrorLogMs_ = 0;
    uint32_t sentEvents_ = 0;
    uint32_t sentStatuses_ = 0;
    uint32_t telemetryWritesTotal_ = 0;
    uint32_t telemetryWritesPerSecond_ = 0;
    uint32_t lastShotCount_ = 0;
    uint32_t failedSends_ = 0;
    uint32_t droppedEvents_ = 0;
    WifiTelemetry telemetry_;
    uint16_t windowMicPeakToPeak_ = 0;
    float windowMicRms_ = 0.0F;
    float windowVibrationMagnitude_ = 0.0F;
    float windowVibrationJerk_ = 0.0F;
    bool enabled_ = false;
    bool lowPowerMode_ = false;
    bool radioEnabled_ = false;
    bool wasConnected_ = false;
    bool recordInFlight_ = false;
    bool ackListenerStarted_ = false;
};
