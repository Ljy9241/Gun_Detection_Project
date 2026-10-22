#include "wifi_transport.h"

#include "system_config.h"

void WifiTransport::begin() {
    enabled_ = config::WIFI_SSID[0] != '\0' && config::UPPER_COMPUTER_HOST[0] != '\0';
    if (!enabled_) {
        Serial.println("Wi-Fi upload disabled: configure WIFI_SSID and UPPER_COMPUTER_HOST.");
        return;
    }
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(config::WIFI_SSID, config::WIFI_PASSWORD);
    lastConnectAttemptMs_ = millis();
    Serial.printf("Wi-Fi connecting: ssid=%s, UDP target=%s:%u\n", config::WIFI_SSID,
                  config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT);
}

void WifiTransport::setTelemetry(const WifiTelemetry& telemetry) {
    telemetry_ = telemetry;
    // 在状态包间隔内累计峰值；成功发送后才清零，发送失败则保留供下次上报。
    windowMicPeakToPeak_ = max(windowMicPeakToPeak_, telemetry.micPeakToPeak);
    windowMicRms_ = max(windowMicRms_, telemetry.micRms);
    windowVibrationMagnitude_ = max(windowVibrationMagnitude_, telemetry.vibrationMagnitude);
    windowVibrationJerk_ = max(windowVibrationJerk_, telemetry.vibrationJerk);
}

void WifiTransport::update(uint32_t nowMs) {
    if (!enabled_) {
        return;
    }

    const bool isConnected = WiFi.status() == WL_CONNECTED;
    if (isConnected != wasConnected_) {
        wasConnected_ = isConnected;
        if (isConnected) {
            Serial.printf("Wi-Fi connected: local=%s, UDP target=%s:%u\n",
                          WiFi.localIP().toString().c_str(), config::UPPER_COMPUTER_HOST,
                          config::UPPER_COMPUTER_PORT);
        } else {
            Serial.println("Wi-Fi disconnected; UDP upload paused.");
        }
    }

    // 未连接时按间隔重连；连接恢复后再发送队列和周期状态。
    if (!isConnected) {
        if (nowMs - lastConnectAttemptMs_ >= config::WIFI_RECONNECT_INTERVAL_MS) {
            lastConnectAttemptMs_ = nowMs;
            WiFi.disconnect();
            WiFi.begin(config::WIFI_SSID, config::WIFI_PASSWORD);
        }
        return;
    }

    if (count_ > 0 && send(queue_[tail_])) {
        tail_ = (tail_ + 1) % QUEUE_SIZE;
        --count_;
    }

    if (nowMs - lastStatusMs_ >= config::UDP_STATUS_INTERVAL_MS) {
        lastStatusMs_ = nowMs;
        if (sendStatus(nowMs)) {
            windowMicPeakToPeak_ = 0;
            windowMicRms_ = 0.0F;
            windowVibrationMagnitude_ = 0.0F;
            windowVibrationJerk_ = 0.0F;
        }
    }
}

bool WifiTransport::sendStatus(uint32_t nowMs) {
    // 状态包使用 JSON，字段结构与 data_show/gun_wifi_monitor.py 的解析保持一致。
    char payload[768];
    const String localIp = WiFi.localIP().toString();
    const int length = snprintf(
        payload, sizeof(payload),
        "{\"type\":\"status\",\"deviceId\":\"%s\",\"uptimeMs\":%lu,"
        "\"localIp\":\"%s\",\"rssi\":%ld,"
        "\"mic\":{\"raw\":%u,\"p2p\":%u,\"rms\":%.1f,"
        "\"maxP2P5s\":%u,\"maxRms5s\":%.1f},"
        "\"vibration\":{\"ready\":%s,\"x\":%u,\"y\":%u,\"z\":%u,"
        "\"magnitude\":%.1f,\"jerk\":%.1f,\"maxMagnitude5s\":%.1f,"
        "\"maxJerk5s\":%.1f},"
        "\"flags\":{\"voice\":%s,\"vibration\":%s},"
        "\"shotCount\":%lu,\"sentShots\":%lu,"
        "\"udp\":{\"queued\":%u,\"failed\":%lu,\"dropped\":%lu},"
        "\"gps\":{\"valid\":%s,\"latitude\":%.7f,\"longitude\":%.7f,"
        "\"altitudeM\":%.1f,\"speedKmph\":%.1f,\"hdop\":%.1f,"
        "\"satellites\":%lu,\"ageMs\":%lu,\"chars\":%lu,"
        "\"failedChecksums\":%lu},"
        "\"sampleDrops\":{\"voice\":%lu,\"vibration\":%lu}}\r\n",
        config::DEVICE_ID, static_cast<unsigned long>(nowMs), localIp.c_str(),
        static_cast<long>(WiFi.RSSI()), telemetry_.micRaw, telemetry_.micPeakToPeak,
        telemetry_.micRms, windowMicPeakToPeak_, windowMicRms_,
        telemetry_.vibrationReady ? "true" : "false", telemetry_.vibrationX,
        telemetry_.vibrationY, telemetry_.vibrationZ, telemetry_.vibrationMagnitude,
        telemetry_.vibrationJerk, windowVibrationMagnitude_, windowVibrationJerk_,
        telemetry_.voiceFlag ? "true" : "false",
        telemetry_.vibrationFlag ? "true" : "false",
        static_cast<unsigned long>(telemetry_.shotCount),
        static_cast<unsigned long>(sentEvents_), count_,
        static_cast<unsigned long>(failedSends_),
        static_cast<unsigned long>(droppedEvents_),
        telemetry_.gpsValid ? "true" : "false",
        telemetry_.latitude, telemetry_.longitude, telemetry_.altitudeM,
        telemetry_.speedKmph, telemetry_.hdop,
        static_cast<unsigned long>(telemetry_.satellites),
        static_cast<unsigned long>(telemetry_.gpsAgeMs),
        static_cast<unsigned long>(telemetry_.gpsCharacters),
        static_cast<unsigned long>(telemetry_.gpsFailedChecksums),
        static_cast<unsigned long>(telemetry_.voiceDroppedSamples),
        static_cast<unsigned long>(telemetry_.vibrationDroppedSamples));

    if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload) ||
        !udp_.beginPacket(config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT)) {
        ++failedSends_;
        Serial.printf("UDP STATUS failed -> %s:%u\n", config::UPPER_COMPUTER_HOST,
                      config::UPPER_COMPUTER_PORT);
        return false;
    }

    udp_.write(reinterpret_cast<const uint8_t*>(payload), length);
    if (udp_.endPacket() != 1) {
        ++failedSends_;
        Serial.printf("UDP STATUS failed -> %s:%u\n", config::UPPER_COMPUTER_HOST,
                      config::UPPER_COMPUTER_PORT);
        return false;
    }

    ++sentStatuses_;
    Serial.printf("UDP STATUS OK -> %s:%u bytes=%d: %s\n",
                  config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT, length, payload);
    return true;
}

bool WifiTransport::enqueue(const ShotEvent& event) {
    if (!enabled_) {
        return false;
    }
    if (count_ >= QUEUE_SIZE) {
        ++droppedEvents_;
        Serial.printf("UDP queue full: dropped shotId=%lu, totalDropped=%lu\n",
                      static_cast<unsigned long>(event.shotId),
                      static_cast<unsigned long>(droppedEvents_));
        return false;
    }
    // 环形队列满时丢弃新事件；否则按 FIFO 顺序等待网络发送。
    queue_[head_] = event;
    head_ = (head_ + 1) % QUEUE_SIZE;
    ++count_;
    lastShotCount_ = event.shotCount;
    return true;
}

bool WifiTransport::send(const ShotEvent& event) {
    // 射击包保留事件发生时的传感器特征和定位信息，便于上位机记录。
    char payload[512];
    const int length = snprintf(
        payload, sizeof(payload),
        "{\"type\":\"shot\",\"deviceId\":\"%s\",\"shotId\":%lu,"
        "\"shotCount\":%lu,\"uptimeUs\":%llu,"
        "\"voiceP2P\":%u,\"voiceRms\":%.1f,\"voiceRiseRatio\":%.2f,"
        "\"accelX\":%u,\"accelY\":%u,\"accelZ\":%u,"
        "\"accelPeak\":%.1f,\"accelJerk\":%.1f,\"accelSaturated\":%s,"
        "\"gpsValid\":%s,\"latitude\":%.7f,\"longitude\":%.7f,\"altitudeM\":%.1f,"
        "\"speedKmph\":%.1f,\"hdop\":%.1f,\"satellites\":%lu,\"gpsAgeMs\":%lu}\r\n",
        config::DEVICE_ID, static_cast<unsigned long>(event.shotId),
        static_cast<unsigned long>(event.shotCount),
        static_cast<unsigned long long>(event.timestampUs), event.voice.peakToPeak,
        event.voice.rms, event.voice.riseRatio, event.vibration.rawX,
        event.vibration.rawY, event.vibration.rawZ, event.vibration.peakMagnitude,
        event.vibration.peakJerk, event.vibration.saturated ? "true" : "false",
        event.gps.valid ? "true" : "false", event.gps.latitude, event.gps.longitude,
        event.gps.altitudeM, event.gps.speedKmph, event.gps.hdop,
        static_cast<unsigned long>(event.gps.satellites),
        static_cast<unsigned long>(event.gps.ageMs));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload)) {
        ++failedSends_;
        Serial.printf("UDP TX encode failed: shotId=%lu, requiredBytes=%d\n",
                      static_cast<unsigned long>(event.shotId), length);
        return false;
    }

    if (!udp_.beginPacket(config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT)) {
        ++failedSends_;
        const uint32_t nowMs = millis();
        if (nowMs - lastSendErrorLogMs_ >= 1000) {
            lastSendErrorLogMs_ = nowMs;
            Serial.printf("UDP TX failed: beginPacket %s:%u, shotId=%lu\n",
                          config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT,
                          static_cast<unsigned long>(event.shotId));
        }
        return false;
    }
    udp_.write(reinterpret_cast<const uint8_t*>(payload), length);
    if (udp_.endPacket() != 1) {
        ++failedSends_;
        const uint32_t nowMs = millis();
        if (nowMs - lastSendErrorLogMs_ >= 1000) {
            lastSendErrorLogMs_ = nowMs;
            Serial.printf("UDP TX failed: endPacket %s:%u, shotId=%lu\n",
                          config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT,
                          static_cast<unsigned long>(event.shotId));
        }
        return false;
    }

    ++sentEvents_;
    Serial.printf("UDP TX OK -> %s:%u bytes=%d: %s\n", config::UPPER_COMPUTER_HOST,
                  config::UPPER_COMPUTER_PORT, length, payload);
    return true;
}
