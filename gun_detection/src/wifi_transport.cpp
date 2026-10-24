#include "wifi_transport.h"

#include <esp_timer.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "system_config.h"

static bool formatTimestamp(char* buffer, size_t size) {
    const time_t now = time(nullptr);
    if (now >= 1704067200) {  // 2024-01-01；早于此值表示尚无有效日历基准。
        // 系统时钟统一保存 UTC 时间；仅在生成显示/存储字符串时换算为北京时间。
        const time_t beijingTime = now + 8 * 60 * 60;
        tm beijing = {};
        if (gmtime_r(&beijingTime, &beijing) &&
            strftime(buffer, size, "%Y-%m-%d %H:%M:%S", &beijing) > 0) {
            return true;
        }
    }

    // GPS/NTP 尚未校时：用单调计时器记录本次上电后的经过时间，不伪造日历时间。
    const uint64_t elapsedSeconds = static_cast<uint64_t>(esp_timer_get_time()) / 1000000ULL;
    const uint64_t hours = elapsedSeconds / 3600ULL;
    const uint64_t minutes = (elapsedSeconds / 60ULL) % 60ULL;
    const uint64_t seconds = elapsedSeconds % 60ULL;
    snprintf(buffer, size, "T+%02llu:%02llu:%02llu",
             static_cast<unsigned long long>(hours),
             static_cast<unsigned long long>(minutes),
             static_cast<unsigned long long>(seconds));
    return true;
}

void WifiTransport::begin() {
    spool_.begin();
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
    if (nowMs - lastSpoolMs_ >= config::UDP_STATUS_INTERVAL_MS) {
        lastSpoolMs_ = nowMs;
        TelemetryRecord record;
        formatTimestamp(record.timestamp, sizeof(record.timestamp));
        record.accelX = telemetry_.vibrationX;
        record.accelY = telemetry_.vibrationY;
        record.accelZ = telemetry_.vibrationZ;
        record.micRaw = telemetry_.micRaw;
        record.micP2p = telemetry_.micPeakToPeak;
        record.micRms = telemetry_.micRms;
        record.latitude = telemetry_.latitude;
        record.longitude = telemetry_.longitude;
        record.gpsValid = telemetry_.gpsValid ? 1 : 0;
        spool_.append(record);
    }
    if (!enabled_) {
        return;
    }

    const bool isConnected = WiFi.status() == WL_CONNECTED;
    if (isConnected != wasConnected_) {
        wasConnected_ = isConnected;
        if (isConnected) {
            // NTP 使用 UTC 校准系统时钟；时间戳格式化时再转换为北京时间。
            configTime(0, 0, config::NTP_SERVER);
            Serial.printf("NTP time synchronization started: %s\n", config::NTP_SERVER);
            ackListenerStarted_ = udp_.begin(config::DEVICE_UDP_PORT);
            if (!ackListenerStarted_) {
                Serial.printf("UDP ACK listener failed on port %u.\n", config::DEVICE_UDP_PORT);
            }
            Serial.printf("Wi-Fi connected: local=%s, UDP target=%s:%u\n",
                          WiFi.localIP().toString().c_str(), config::UPPER_COMPUTER_HOST,
                          config::UPPER_COMPUTER_PORT);
        } else {
            udp_.stop();
            ackListenerStarted_ = false;
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

    if (!ackListenerStarted_) {
        ackListenerStarted_ = udp_.begin(config::DEVICE_UDP_PORT);
        if (!ackListenerStarted_) return;
    }

    receiveAcknowledgements();
    if (!recordInFlight_ || nowMs - lastRecordSendMs_ >= 500) {
        TelemetryRecord records[TELEMETRY_BATCH_SIZE];
        const size_t count = spool_.peekBatch(records, TELEMETRY_BATCH_SIZE);
        if (count > 0 && sendTelemetryBatch(records, count)) {
            inFlightCount_ = static_cast<uint8_t>(count);
            for (size_t i = 0; i < count; ++i) inFlightSequences_[i] = records[i].sequence;
            recordInFlight_ = true;
            lastRecordSendMs_ = nowMs;
        }
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

bool WifiTransport::sendTelemetryBatch(const TelemetryRecord* records, size_t count) {
    // Four records keep JSON datagrams comfortably below the common Ethernet MTU.
    char payload[1600];
    int length = snprintf(payload, sizeof(payload),
                          "{\"type\":\"telemetry_batch\",\"deviceId\":\"%s\",\"records\":[",
                          config::DEVICE_ID);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload)) return false;

    for (size_t i = 0; i < count; ++i) {
        const TelemetryRecord& record = records[i];
        char latitude[24];
        char longitude[24];
        if (record.gpsValid) {
            snprintf(latitude, sizeof(latitude), "%.7f", record.latitude);
            snprintf(longitude, sizeof(longitude), "%.7f", record.longitude);
        } else {
            strcpy(latitude, "null");
            strcpy(longitude, "null");
        }
        const int written = snprintf(
            payload + length, sizeof(payload) - static_cast<size_t>(length),
            "%s{\"sequence\":%llu,\"timestamp\":\"%s\",\"timestampValid\":%s,"
            "\"accel_x\":%u,\"accel_y\":%u,\"accel_z\":%u,"
            "\"mic_raw\":%u,\"mic_p2p\":%u,\"mic_rms\":%.3f,"
            "\"latitude\":%s,\"longitude\":%s}",
            i ? "," : "", static_cast<unsigned long long>(record.sequence), record.timestamp,
            record.timestamp[0] ? "true" : "false", record.accelX, record.accelY, record.accelZ,
            record.micRaw, record.micP2p, record.micRms, latitude, longitude);
        if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload) - static_cast<size_t>(length)) {
            return false;
        }
        length += written;
    }

    const int suffixLength = snprintf(payload + length, sizeof(payload) - static_cast<size_t>(length), "]}");
    if (suffixLength <= 0 || static_cast<size_t>(suffixLength) >= sizeof(payload) - static_cast<size_t>(length) ||
        !udp_.beginPacket(config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT)) return false;
    length += suffixLength;
    udp_.write(reinterpret_cast<const uint8_t*>(payload), length);
    return udp_.endPacket() == 1;
}

void WifiTransport::receiveAcknowledgements() {
    while (udp_.parsePacket() > 0) {
        char payload[256] = {};
        const int length = udp_.read(reinterpret_cast<uint8_t*>(payload), sizeof(payload) - 1);
        if (length <= 0) continue;
        payload[length] = '\0';
        if (!recordInFlight_ || !strstr(payload, "\"type\":\"ack_batch\"") ||
            !strstr(payload, config::DEVICE_ID)) continue;
        char* sequenceField = strstr(payload, "\"sequences\":[");
        if (!sequenceField) continue;
        char* cursor = sequenceField + strlen("\"sequences\":[");
        bool matches = true;
        for (uint8_t i = 0; i < inFlightCount_; ++i) {
            char* end = nullptr;
            const uint64_t sequence = strtoull(cursor, &end, 10);
            if (end == cursor || sequence != inFlightSequences_[i]) {
                matches = false;
                break;
            }
            cursor = end;
            if (i + 1 < inFlightCount_) {
                if (*cursor++ != ',') { matches = false; break; }
            } else if (*cursor != ']') {
                matches = false;
            }
        }
        if (!matches) continue;
        bool acknowledged = true;
        for (uint8_t i = 0; i < inFlightCount_; ++i) {
            if (!spool_.acknowledge(inFlightSequences_[i])) {
                acknowledged = false;
                break;
            }
        }
        if (acknowledged) {
            inFlightCount_ = 0;
            recordInFlight_ = false;
        }
    }
}

bool WifiTransport::sendStatus(uint32_t nowMs) {
    // 状态包使用 JSON，字段结构与 data_show/gun_wifi_monitor.py 的解析保持一致。
    char payload[1024];
    char timestamp[20];
    const bool timestampValid = formatTimestamp(timestamp, sizeof(timestamp));
    const String localIp = WiFi.localIP().toString();
    const int length = snprintf(
        payload, sizeof(payload),
        "{\"type\":\"status\",\"deviceId\":\"%s\",\"timestamp\":\"%s\","
        "\"timestampValid\":%s,\"uptimeMs\":%lu,"
        "\"localIp\":\"%s\",\"rssi\":%ld,"
        "\"mic\":{\"raw\":%u,\"p2p\":%u,\"rms\":%.1f,"
        "\"maxP2P5s\":%u,\"maxRms5s\":%.1f},"
        "\"vibration\":{\"ready\":%s,\"x\":%u,\"y\":%u,\"z\":%u,"
        "\"magnitude\":%.1f,\"jerk\":%.1f,\"maxMagnitude5s\":%.1f,"
        "\"maxJerk5s\":%.1f},"
        "\"tilt\":{\"valid\":%s,\"rollDeg\":%.1f,\"pitchDeg\":%.1f},"
        "\"flags\":{\"voice\":%s,\"vibration\":%s},"
        "\"shotCount\":%lu,\"voiceDetectionCount\":%lu,"
        "\"vibrationDetectionCount\":%lu,\"sentShots\":%lu,"
        "\"udp\":{\"queued\":%u,\"failed\":%lu,\"dropped\":%lu},"
        "\"gps\":{\"valid\":%s,\"latitude\":%.7f,\"longitude\":%.7f,"
        "\"altitudeM\":%.1f,\"speedKmph\":%.1f,\"hdop\":%.1f,"
        "\"satellites\":%lu,\"ageMs\":%lu,\"chars\":%lu,"
        "\"failedChecksums\":%lu},"
        "\"sampleDrops\":{\"voice\":%lu,\"vibration\":%lu}}\r\n",
        config::DEVICE_ID, timestamp, timestampValid ? "true" : "false",
        static_cast<unsigned long>(nowMs), localIp.c_str(),
        static_cast<long>(WiFi.RSSI()), telemetry_.micRaw, telemetry_.micPeakToPeak,
        telemetry_.micRms, windowMicPeakToPeak_, windowMicRms_,
        telemetry_.vibrationReady ? "true" : "false", telemetry_.vibrationX,
        telemetry_.vibrationY, telemetry_.vibrationZ, telemetry_.vibrationMagnitude,
        telemetry_.vibrationJerk, windowVibrationMagnitude_, windowVibrationJerk_,
        telemetry_.tiltValid ? "true" : "false", telemetry_.rollDegrees,
        telemetry_.pitchDegrees,
        telemetry_.voiceFlag ? "true" : "false",
        telemetry_.vibrationFlag ? "true" : "false",
        static_cast<unsigned long>(telemetry_.shotCount),
        static_cast<unsigned long>(telemetry_.voiceDetectionCount),
        static_cast<unsigned long>(telemetry_.vibrationDetectionCount),
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
    QueuedEvent queued;
    queued.type = EventType::Shot;
    queued.shot = event;
    formatTimestamp(queued.timestamp, sizeof(queued.timestamp));
    if (!enqueueEvent(queued)) return false;
    lastShotCount_ = event.shotCount;
    return true;
}

bool WifiTransport::enqueueVoiceDetection(const VoiceEvent& event, uint32_t detectionCount) {
    QueuedEvent queued;
    queued.type = EventType::Voice;
    queued.voice = event;
    queued.detectionCount = detectionCount;
    formatTimestamp(queued.timestamp, sizeof(queued.timestamp));
    return enqueueEvent(queued);
}

bool WifiTransport::enqueueVibrationDetection(const VibrateEvent& event,
                                               uint32_t detectionCount) {
    QueuedEvent queued;
    queued.type = EventType::Vibration;
    queued.vibration = event;
    queued.detectionCount = detectionCount;
    formatTimestamp(queued.timestamp, sizeof(queued.timestamp));
    return enqueueEvent(queued);
}

bool WifiTransport::enqueueEvent(const QueuedEvent& event) {
    if (!enabled_) return false;
    if (count_ >= QUEUE_SIZE) {
        ++droppedEvents_;
        Serial.printf("UDP queue full: dropped event, totalDropped=%lu\n",
                      static_cast<unsigned long>(droppedEvents_));
        return false;
    }
    queue_[head_] = event;
    head_ = (head_ + 1) % QUEUE_SIZE;
    ++count_;
    return true;
}

bool WifiTransport::send(const QueuedEvent& event) {
    char payload[512];
    int length = 0;
    if (event.type == EventType::Shot) {
        const ShotEvent& shot = event.shot;
        length = snprintf(
            payload, sizeof(payload),
            "{\"type\":\"shot\",\"deviceId\":\"%s\",\"timestamp\":\"%s\","
            "\"timestampValid\":%s,\"shotId\":%lu,\"shotCount\":%lu,\"uptimeUs\":%llu,"
            "\"voiceP2P\":%u,\"voiceRms\":%.1f,\"voiceRiseRatio\":%.2f,"
            "\"accelX\":%u,\"accelY\":%u,\"accelZ\":%u,\"accelPeak\":%.1f,"
            "\"accelJerk\":%.1f,\"accelSaturated\":%s,\"gpsValid\":%s,"
            "\"latitude\":%.7f,\"longitude\":%.7f,\"altitudeM\":%.1f,"
            "\"speedKmph\":%.1f,\"hdop\":%.1f,\"satellites\":%lu,\"gpsAgeMs\":%lu}\r\n",
            config::DEVICE_ID, event.timestamp, event.timestamp[0] ? "true" : "false",
            static_cast<unsigned long>(shot.shotId), static_cast<unsigned long>(shot.shotCount),
            static_cast<unsigned long long>(shot.timestampUs), shot.voice.peakToPeak,
            shot.voice.rms, shot.voice.riseRatio, shot.vibration.rawX, shot.vibration.rawY,
            shot.vibration.rawZ, shot.vibration.peakMagnitude, shot.vibration.peakJerk,
            shot.vibration.saturated ? "true" : "false", shot.gps.valid ? "true" : "false",
            shot.gps.latitude, shot.gps.longitude, shot.gps.altitudeM, shot.gps.speedKmph,
            shot.gps.hdop, static_cast<unsigned long>(shot.gps.satellites),
            static_cast<unsigned long>(shot.gps.ageMs));
    } else if (event.type == EventType::Voice) {
        length = snprintf(payload, sizeof(payload),
            "{\"type\":\"detection\",\"detectionType\":\"voice\",\"deviceId\":\"%s\","
            "\"timestamp\":\"%s\",\"timestampValid\":%s,\"detectionCount\":%lu,"
            "\"uptimeUs\":%llu,\"peakToPeak\":%u,\"rms\":%.1f,\"riseRatio\":%.2f}\r\n",
            config::DEVICE_ID, event.timestamp, event.timestamp[0] ? "true" : "false",
            static_cast<unsigned long>(event.detectionCount),
            static_cast<unsigned long long>(event.voice.timestampUs), event.voice.peakToPeak,
            event.voice.rms, event.voice.riseRatio);
    } else {
        length = snprintf(payload, sizeof(payload),
            "{\"type\":\"detection\",\"detectionType\":\"vibration\",\"deviceId\":\"%s\","
            "\"timestamp\":\"%s\",\"timestampValid\":%s,\"detectionCount\":%lu,"
            "\"uptimeUs\":%llu,\"x\":%u,\"y\":%u,\"z\":%u,"
            "\"peakMagnitude\":%.1f,\"peakJerk\":%.1f,\"saturated\":%s}\r\n",
            config::DEVICE_ID, event.timestamp, event.timestamp[0] ? "true" : "false",
            static_cast<unsigned long>(event.detectionCount),
            static_cast<unsigned long long>(event.vibration.timestampUs), event.vibration.rawX,
            event.vibration.rawY, event.vibration.rawZ, event.vibration.peakMagnitude,
            event.vibration.peakJerk, event.vibration.saturated ? "true" : "false");
    }
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(payload)) {
        ++failedSends_;
        Serial.printf("UDP TX encode failed: event type=%u, requiredBytes=%d\n",
                      static_cast<unsigned>(event.type), length);
        return false;
    }

    if (!udp_.beginPacket(config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT)) {
        ++failedSends_;
        const uint32_t nowMs = millis();
        if (nowMs - lastSendErrorLogMs_ >= 1000) {
            lastSendErrorLogMs_ = nowMs;
            Serial.printf("UDP TX failed: beginPacket %s:%u, event type=%u\n",
                          config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT,
                          static_cast<unsigned>(event.type));
        }
        return false;
    }
    udp_.write(reinterpret_cast<const uint8_t*>(payload), length);
    if (udp_.endPacket() != 1) {
        ++failedSends_;
        const uint32_t nowMs = millis();
        if (nowMs - lastSendErrorLogMs_ >= 1000) {
            lastSendErrorLogMs_ = nowMs;
            Serial.printf("UDP TX failed: endPacket %s:%u, event type=%u\n",
                          config::UPPER_COMPUTER_HOST, config::UPPER_COMPUTER_PORT,
                          static_cast<unsigned>(event.type));
        }
        return false;
    }

    ++sentEvents_;
    Serial.printf("UDP TX OK -> %s:%u bytes=%d: %s\n", config::UPPER_COMPUTER_HOST,
                  config::UPPER_COMPUTER_PORT, length, payload);
    return true;
}
