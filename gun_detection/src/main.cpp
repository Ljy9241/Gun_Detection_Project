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
uint32_t voiceDetectionCount = 0;
uint32_t vibrationDetectionCount = 0;
bool lowPowerMode = false;
uint64_t stationarySinceUs = 0;
bool lowPowerGpsWindowActive = false;
uint32_t lowPowerGpsWindowStartMs = 0;
uint32_t nextLowPowerGpsStartMs = 0;

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

    if (!config::CAPACITY_TEST_MODE && vibrateDetection.calibrated()) {
        const bool stationary = vibrateDetection.magnitude() <
                                config::LOW_POWER_STATIONARY_ACCEL_THRESHOLD_COUNTS;
        if (!lowPowerMode) {
            if (stationary) {
                if (stationarySinceUs == 0) stationarySinceUs = nowUs;
                if (nowUs - stationarySinceUs >=
                    static_cast<uint64_t>(config::LOW_POWER_STILLNESS_MS) * 1000ULL) {
                    lowPowerMode = true;
                    nextLowPowerGpsStartMs = nowMs;  // 首个 GPS 窗口立即启动。
                    lowPowerGpsWindowActive = false;
                    wifiTransport.setLowPowerMode(true, nowMs);
                    Serial.println("Stillness >5s; entering low-power schedule.");
                }
            } else {
                stationarySinceUs = 0;
            }
        } else if (!stationary) {
            lowPowerMode = false;
            stationarySinceUs = 0;
            lowPowerGpsWindowActive = false;
            gps.setEnabled(true);
            wifiTransport.setLowPowerMode(false, nowMs);
            Serial.println("Motion detected; normal mode restored.");
        }
    }

    if (lowPowerMode) {
        if (!lowPowerGpsWindowActive &&
            static_cast<int32_t>(nowMs - nextLowPowerGpsStartMs) >= 0) {
            gps.setEnabled(true);
            lowPowerGpsWindowActive = true;
            lowPowerGpsWindowStartMs = nowMs;
            nextLowPowerGpsStartMs += config::LOW_POWER_GPS_CYCLE_MS;
            Serial.println("Low-power GPS window started.");
        }
        if (lowPowerGpsWindowActive) {
            const GpsFix windowFix = gps.currentFix();
            const bool gotFix = windowFix.valid;
            const bool timedOut = nowMs - lowPowerGpsWindowStartMs >= config::LOW_POWER_GPS_WINDOW_MS;
            if (gotFix || timedOut) {
                gps.setEnabled(false);
                lowPowerGpsWindowActive = false;
                Serial.println(gotFix ? "Low-power GPS fix acquired; GPS off."
                                      : "Low-power GPS timed out after 5 minutes; GPS off.");
            }
        }
    } else {
        gps.setEnabled(true);
    }
    gps.update();

    VoiceEvent voiceEvent;
    if (voiceDetection.consumeEvent(voiceEvent)) {
        ++voiceDetectionCount;
        Serial.printf("VOICE TRIGGER p2p=%u rms=%.1f rise=%.2f\n", voiceEvent.peakToPeak,
                      voiceEvent.rms, voiceEvent.riseRatio);
        wifiTransport.enqueueVoiceDetection(voiceEvent, voiceDetectionCount);
        shotDetector.onVoiceEvent(voiceEvent);
    }

    VibrateEvent vibrateEvent;
    if (vibrateDetection.consumeEvent(vibrateEvent)) {
        ++vibrationDetectionCount;
        Serial.printf("VIB TRIGGER peak=%.1f jerk=%.1f sat=%s\n",
                      vibrateEvent.peakMagnitude, vibrateEvent.peakJerk,
                      vibrateEvent.saturated ? "yes" : "no");
        wifiTransport.enqueueVibrationDetection(vibrateEvent, vibrationDetectionCount);
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
    telemetry.tiltValid = vibrateDetection.angleValid();
    telemetry.rollDegrees = vibrateDetection.rollDegrees();
    telemetry.pitchDegrees = vibrateDetection.pitchDegrees();
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
    telemetry.voiceDetectionCount = voiceDetectionCount;
    telemetry.vibrationDetectionCount = vibrationDetectionCount;
    telemetry.voiceDroppedSamples = voiceDetection.droppedSamples();
    telemetry.vibrationDroppedSamples = vibrateDetection.droppedSamples();
    wifiTransport.setTelemetry(telemetry);
    wifiTransport.update(nowMs);

    // 定期输出健康状态和丢样/发送计数，便于现场排查。
    if (nowMs - lastDiagnosticMs >= config::DIAGNOSTIC_INTERVAL_MS) {
        lastDiagnosticMs = nowMs;
        Serial.printf(
            "mic[raw=%u p2p=%u rms=%.1f] mma[%s x=%u y=%u z=%u mag=%.1f jerk=%.1f] "
            "flags[%d,%d] power[%s] gps[%s sats=%lu chars=%lu] wifi[%s] "
            "spool[written=%lu rate=%lu/s] udp[q=%u shot=%lu status=%lu fail=%lu drop=%lu] "
            "dropped[%lu,%lu]\n",
            voiceDetection.latestRaw(), voiceDetection.latestPeakToPeak(),
            voiceDetection.latestRms(), vibrateDetection.calibrated() ? "ready" : "cal",
            vibrateDetection.rawX(), vibrateDetection.rawY(), vibrateDetection.rawZ(),
            vibrateDetection.magnitude(), vibrateDetection.jerk(), shotDetector.flag1(),
            shotDetector.flag2(), lowPowerMode ? "low" : "normal", fix.valid ? "fix" : "search",
            static_cast<unsigned long>(fix.satellites),
            static_cast<unsigned long>(gps.parsedCharacters()),
            wifiTransport.connected() ? "connected" :
                (wifiTransport.radioEnabled() ? "search" : "off"),
            static_cast<unsigned long>(wifiTransport.telemetryWritesTotal()),
            static_cast<unsigned long>(wifiTransport.telemetryWritesPerSecond()),
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
