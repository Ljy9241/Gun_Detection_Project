#include <Arduino.h>
#include <esp_timer.h>

#include "gps_manager.h"
#include "shot_detector.h"
#include "system_config.h"
#include "vibrate_detection.h"
#include "voice_detection.h"
#include "wifi_transport.h"

// 各功能模块由主循环协同调度，传感器模块自身不创建任务。
VoiceDetection voiceDetection;
VibrateDetection vibrateDetection;
GpsManager gps(Serial1);
ShotDetector shotDetector;
WifiTransport wifiTransport;

uint32_t lastDiagnosticMs = 0;

static void printShot(const ShotEvent& shot) {
    // 输出融合事件摘要，便于不连接上位机时通过串口检查检测结果。
    Serial.printf("SHOT #%lu voice[p2p=%u rms=%.1f] vibration[peak=%.1f jerk=%.1f sat=%s] ",
                  static_cast<unsigned long>(shot.shotCount), shot.voice.peakToPeak,
                  shot.voice.rms, shot.vibration.peakMagnitude, shot.vibration.peakJerk,
                  shot.vibration.saturated ? "yes" : "no");
    if (shot.gps.valid) {
        Serial.printf("gps[%.7f,%.7f age=%lums]\n", shot.gps.latitude, shot.gps.longitude,
                      static_cast<unsigned long>(shot.gps.ageMs));
    } else {
        Serial.println("gps[no fix]");
    }
}

void setup() {
    Serial.begin(config::SERIAL_BAUD);
    delay(500);
    Serial.println();
    Serial.println("Gun detection firmware starting...");

    analogReadResolution(12);
    voiceDetection.begin(config::MIC_ADC_PIN);
    vibrateDetection.begin(config::MMA_X_PIN, config::MMA_Y_PIN, config::MMA_Z_PIN,
                            config::MMA_SLEEP_PIN);
    gps.begin(config::GPS_ENABLE_PIN, config::GPS_PPS_PIN, config::GPS_UART_RX_PIN,
              config::GPS_UART_TX_PIN, config::GPS_BAUD);
    wifiTransport.begin();

    Serial.println("MMA7361 and GPS enabled; continuous detection started.");
}

void loop() {
    // 统一使用微秒时钟驱动传感器采样和事件融合，毫秒时钟用于联网周期任务。
    const uint64_t nowUs = esp_timer_get_time();
    const uint32_t nowMs = millis();

    voiceDetection.update(nowUs);
    vibrateDetection.update(nowUs);
    gps.update();

    VoiceEvent voiceEvent;
    if (voiceDetection.consumeEvent(voiceEvent)) {
        Serial.printf("VOICE TRIGGER p2p=%u rms=%.1f rise=%.2f\n", voiceEvent.peakToPeak,
                      voiceEvent.rms, voiceEvent.riseRatio);
        shotDetector.onVoiceEvent(voiceEvent);
    }

    VibrateEvent vibrateEvent;
    if (vibrateDetection.consumeEvent(vibrateEvent)) {
        Serial.printf("VIB TRIGGER peak=%.1f jerk=%.1f sat=%s\n",
                      vibrateEvent.peakMagnitude, vibrateEvent.peakJerk,
                      vibrateEvent.saturated ? "yes" : "no");
        shotDetector.onVibrateEvent(vibrateEvent);
    }

    shotDetector.update(nowUs);
    const GpsFix fix = gps.currentFix();
    ShotEvent shot;
    if (shotDetector.consumeShot(shot, fix)) {
        printShot(shot);
        wifiTransport.enqueue(shot);
    }

    // 汇总最新传感器状态；WifiTransport 会在自己的周期内打包上报。
    WifiTelemetry telemetry;
    telemetry.micRaw = voiceDetection.latestRaw();
    telemetry.micPeakToPeak = voiceDetection.latestPeakToPeak();
    telemetry.micRms = voiceDetection.latestRms();
    telemetry.vibrationReady = vibrateDetection.calibrated();
    telemetry.vibrationX = vibrateDetection.rawX();
    telemetry.vibrationY = vibrateDetection.rawY();
    telemetry.vibrationZ = vibrateDetection.rawZ();
    telemetry.vibrationMagnitude = vibrateDetection.magnitude();
    telemetry.vibrationJerk = vibrateDetection.jerk();
    telemetry.voiceFlag = shotDetector.flag1();
    telemetry.vibrationFlag = shotDetector.flag2();
    telemetry.gpsValid = fix.valid;
    telemetry.latitude = fix.latitude;
    telemetry.longitude = fix.longitude;
    telemetry.altitudeM = fix.altitudeM;
    telemetry.speedKmph = fix.speedKmph;
    telemetry.hdop = fix.hdop;
    telemetry.satellites = fix.satellites;
    telemetry.gpsAgeMs = fix.ageMs;
    telemetry.gpsCharacters = gps.parsedCharacters();
    telemetry.gpsFailedChecksums = gps.failedChecksums();
    telemetry.shotCount = shotDetector.shotCount();
    telemetry.voiceDroppedSamples = voiceDetection.droppedSamples();
    telemetry.vibrationDroppedSamples = vibrateDetection.droppedSamples();
    wifiTransport.setTelemetry(telemetry);
    wifiTransport.update(nowMs);

    // 定期输出健康状态和丢样/发送计数，便于现场排查。
    if (nowMs - lastDiagnosticMs >= config::DIAGNOSTIC_INTERVAL_MS) {
        lastDiagnosticMs = nowMs;
        Serial.printf(
            "mic[raw=%u p2p=%u rms=%.1f] mma[%s x=%u y=%u z=%u mag=%.1f jerk=%.1f] "
            "flags[%d,%d] gps[%s sats=%lu chars=%lu] wifi[%s] "
            "udp[q=%u shot=%lu status=%lu fail=%lu drop=%lu] dropped[%lu,%lu]\n",
            voiceDetection.latestRaw(), voiceDetection.latestPeakToPeak(),
            voiceDetection.latestRms(), vibrateDetection.calibrated() ? "ready" : "cal",
            vibrateDetection.rawX(), vibrateDetection.rawY(), vibrateDetection.rawZ(),
            vibrateDetection.magnitude(), vibrateDetection.jerk(), shotDetector.flag1(),
            shotDetector.flag2(), fix.valid ? "fix" : "search",
            static_cast<unsigned long>(fix.satellites),
            static_cast<unsigned long>(gps.parsedCharacters()),
            wifiTransport.connected() ? "connected" : (wifiTransport.enabled() ? "search" : "off"),
            wifiTransport.queuedEvents(),
            static_cast<unsigned long>(wifiTransport.sentEvents()),
            static_cast<unsigned long>(wifiTransport.sentStatuses()),
            static_cast<unsigned long>(wifiTransport.failedSends()),
            static_cast<unsigned long>(wifiTransport.droppedEvents()),
            static_cast<unsigned long>(voiceDetection.droppedSamples()),
            static_cast<unsigned long>(vibrateDetection.droppedSamples()));
    }

    yield();
}
