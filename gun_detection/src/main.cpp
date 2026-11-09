#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "adc_sampler.h"
#include "gps_manager.h"
#include "gps_cycle.h"
#include "shot_detector.h"
#include "low_power_motion_wake.h"
#include "system_config.h"
#include "vibrate_detection.h"
#include "voice_detection.h"
#include "wifi_transport.h"

// 网络与射击记录由主循环调度；高优先级 ADC 任务持续识别动作。
VoiceDetection voiceDetection;
VibrateDetection vibrateDetection;
MotionDirectionDetector motionDetection;
GpsManager gps(Serial1);
ShotDetector shotDetector;
WifiTransport wifiTransport;
AdcSampler adcSampler;
AdcSample adcSamples[1024];
portMUX_TYPE sensorStateMux = portMUX_INITIALIZER_UNLOCKED;
bool adcReaderTaskStarted = false;
std::atomic<bool> pauseSampling{false};
std::atomic<bool> samplingPaused{false};
uint16_t pendingMicSample = 0;
uint16_t pendingAccelX = 0;
uint16_t pendingAccelY = 0;
uint16_t pendingAccelZ = 0;
uint8_t pendingAdcMask = 0;

uint32_t lastDiagnosticMs = 0;
uint32_t voiceDetectionCount = 0;
uint32_t vibrationDetectionCount = 0;
uint32_t verticalMotionCount = 0;  // 本次启动以来识别的完整竖向动作，与联网结果无关。
bool lowPowerMode = false;
LowPowerMotionWake lowPowerMotionWake;
uint64_t stationarySinceUs = 0;
GpsCycle gpsCycle(config::GPS_CYCLE_MS, config::GPS_WINDOW_MS);
bool gpsSnapshotPending = false;
TelemetryRecord gpsSnapshot;
uint32_t lastGpsFlashAttemptMs = 0;

struct SensorSnapshot {
    MotionEvent motionEvent;
    bool hasMotionEvent = false;
    bool classifyingMotion = false;
    VoiceEvent voiceEvent;
    VibrateEvent vibrationEvent;
    uint16_t micRaw = 0;
    uint16_t micPeakToPeak = 0;
    float micRms = 0.0F;
    uint16_t vibrationX = 0;
    uint16_t vibrationY = 0;
    uint16_t vibrationZ = 0;
    float vibrationMagnitude = 0.0F;
    float vibrationJerk = 0.0F;
    float maxAxisSampleDelta = 0.0F;
    float rollDegrees = 0.0F;
    float pitchDegrees = 0.0F;
    uint32_t voiceDroppedSamples = 0;
    uint32_t vibrationDroppedSamples = 0;
    bool vibrationReady = false;
    bool tiltValid = false;
    bool hasVoiceEvent = false;
    bool hasVibrationEvent = false;
};

static void processAdcSample(const AdcSample& sample) {
    switch (sample.input) {
        case AdcInput::Microphone:
            // A new scan begins at the microphone; discard any incomplete
            // previous scan rather than mixing axes from different instants.
            pendingAdcMask = 0;
            pendingMicSample = sample.value;
            pendingAdcMask |= 0x01;
            break;
        case AdcInput::AccelX:
            pendingAccelX = sample.value;
            pendingAdcMask |= 0x02;
            break;
        case AdcInput::AccelY:
            pendingAccelY = sample.value;
            pendingAdcMask |= 0x04;
            break;
        case AdcInput::AccelZ:
            pendingAccelZ = sample.value;
            pendingAdcMask |= 0x08;
            break;
    }
    if (pendingAdcMask == 0x0F) {
        portENTER_CRITICAL(&sensorStateMux);
        voiceDetection.processSample(pendingMicSample, sample.timestampUs);
        vibrateDetection.processSample(pendingAccelX, pendingAccelY,
                                       pendingAccelZ, sample.timestampUs);
        motionDetection.processSample(pendingAccelX, pendingAccelY,
                                      pendingAccelZ, sample.timestampUs);
        portEXIT_CRITICAL(&sensorStateMux);
        pendingAdcMask = 0;
    }
}

static SensorSnapshot captureSensorSnapshot() {
    SensorSnapshot snapshot;
    portENTER_CRITICAL(&sensorStateMux);
    snapshot.hasMotionEvent = motionDetection.consumeEvent(snapshot.motionEvent);
    snapshot.classifyingMotion = motionDetection.classifying();
    snapshot.hasVoiceEvent = voiceDetection.consumeEvent(snapshot.voiceEvent);
    snapshot.hasVibrationEvent = vibrateDetection.consumeEvent(snapshot.vibrationEvent);
    snapshot.micRaw = voiceDetection.latestRaw();
    snapshot.micPeakToPeak = voiceDetection.latestPeakToPeak();
    snapshot.micRms = voiceDetection.latestRms();
    snapshot.vibrationReady = vibrateDetection.calibrated();
    snapshot.vibrationX = vibrateDetection.rawX();
    snapshot.vibrationY = vibrateDetection.rawY();
    snapshot.vibrationZ = vibrateDetection.rawZ();
    snapshot.vibrationMagnitude = vibrateDetection.magnitude();
    snapshot.vibrationJerk = vibrateDetection.jerk();
    snapshot.maxAxisSampleDelta = vibrateDetection.maxAxisSampleDelta();
    snapshot.tiltValid = vibrateDetection.angleValid();
    snapshot.rollDegrees = vibrateDetection.rollDegrees();
    snapshot.pitchDegrees = vibrateDetection.pitchDegrees();
    snapshot.voiceDroppedSamples = voiceDetection.droppedSamples();
    snapshot.vibrationDroppedSamples = vibrateDetection.droppedSamples();
    portEXIT_CRITICAL(&sensorStateMux);
    return snapshot;
}

static void adcReaderTask(void*) {
    // The legacy ADC driver's SAR lock belongs to the task that starts it.
    // Keep start/stop/recovery in this task to avoid cross-task mutex release.
    if (!adcSampler.start()) {
        vTaskDelete(nullptr);
        return;
    }
    for (;;) {
        if (pauseSampling.load()) {
            adcSampler.stop();
            samplingPaused.store(true);
            while (pauseSampling.load()) vTaskDelay(pdMS_TO_TICKS(5));
            portENTER_CRITICAL(&sensorStateMux);
            voiceDetection = VoiceDetection{};
            vibrateDetection = VibrateDetection{};
            motionDetection.reset();
            portEXIT_CRITICAL(&sensorStateMux);
            voiceDetection.begin(config::MIC_ADC_PIN);
            vibrateDetection.begin(config::MMA_X_PIN, config::MMA_Y_PIN, config::MMA_Z_PIN, config::MMA_SLEEP_PIN);
            pendingAdcMask = 0;
            adcSampler.start();
            samplingPaused.store(false);
        }
        const size_t sampleCount = adcSampler.read(adcSamples, 1024, esp_timer_get_time());
        if (sampleCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        for (size_t i = 0; i < sampleCount; ++i) {
            processAdcSample(adcSamples[i]);
        }
        // Let the Arduino loop task service Wi-Fi and GPS between DMA batches.
        // The ADC peripheral continues sampling while this task yields.
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static void printShot(const ShotEvent& shot) {
    Serial.printf("SHOT #%lu source[horizontal_motion] peak[%.1f] energy[%.0f,%.0f,%.0f] ",
                  static_cast<unsigned long>(shot.shotCount), shot.motion.peak,
                  shot.motion.energyX, shot.motion.energyY, shot.motion.energyZ);
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
    Serial.printf("CPU frequency: %lu MHz\n", static_cast<unsigned long>(getCpuFrequencyMhz()));

    voiceDetection.begin(config::MIC_ADC_PIN);
    vibrateDetection.begin(config::MMA_X_PIN, config::MMA_Y_PIN, config::MMA_Z_PIN,
                            config::MMA_SLEEP_PIN);
    adcSampler.begin();
    gps.begin(config::GPS_ENABLE_PIN, config::GPS_PPS_PIN, config::GPS_UART_RX_PIN,
              config::GPS_UART_TX_PIN, config::GPS_BAUD);
    // Start Wi-Fi before launching the high-priority sampler so radio
    // association does not depend on the ADC task yielding during startup.
    wifiTransport.begin();
    gpsCycle.startIfDue(millis());  // First window starts immediately at boot.
    if (adcSampler.ready()) {
        adcReaderTaskStarted = xTaskCreatePinnedToCore(
            adcReaderTask, "adc-reader", 4096, nullptr, 5, nullptr, 1) == pdPASS;
        Serial.printf("ADC reader task: %s.\n", adcReaderTaskStarted ? "started" : "failed");
    }

    Serial.println("MMA7361 and GPS enabled; continuous detection started.");
}

void loop() {
    // 微秒时钟驱动采样和动作识别，毫秒时钟用于联网周期任务。
    const uint64_t nowUs = esp_timer_get_time();
    const uint32_t nowMs = millis();
    if (wifiTransport.uploadOnly()) {
        wifiTransport.update(nowMs);
        pauseSampling.store(true);
        shotDetector.clearPending();
        portENTER_CRITICAL(&sensorStateMux);
        motionDetection.reset();
        portEXIT_CRITICAL(&sensorStateMux);
        gps.setEnabled(false);
        lowPowerMode = false;
        stationarySinceUs = 0;
        gpsCycle.finish();
        delay(1);
        return;
    }
    if (pauseSampling.exchange(false)) {
        if (!adcReaderTaskStarted) {
            voiceDetection = VoiceDetection{};
            vibrateDetection = VibrateDetection{};
            motionDetection.reset();
            voiceDetection.begin(config::MIC_ADC_PIN);
            vibrateDetection.begin(config::MMA_X_PIN, config::MMA_Y_PIN, config::MMA_Z_PIN, config::MMA_SLEEP_PIN);
        }
    }
    if (samplingPaused.load()) { delay(1); return; }

    if (adcSampler.ready()) {
        if (!adcReaderTaskStarted) {
            const size_t sampleCount = adcSampler.read(adcSamples, 1024, nowUs);
            for (size_t i = 0; i < sampleCount; ++i) processAdcSample(adcSamples[i]);
        }
    } else {
        // DMA setup failsafe: retain the previous timed one-shot sampling path.
        voiceDetection.update(nowUs);
        vibrateDetection.update(nowUs);
        motionDetection.processSample(vibrateDetection.rawX(), vibrateDetection.rawY(),
                                      vibrateDetection.rawZ(), nowUs);
    }
    const SensorSnapshot sensors = captureSensorSnapshot();

    if (!config::CAPACITY_TEST_MODE && sensors.vibrationReady) {
        const bool stationary = sensors.maxAxisSampleDelta <=
                                config::LOW_POWER_MOTION_DELTA_THRESHOLD_COUNTS;
        if (!lowPowerMode) {
            if (stationary) {
                if (stationarySinceUs == 0) stationarySinceUs = nowUs;
                if (nowUs - stationarySinceUs >=
                    static_cast<uint64_t>(config::LOW_POWER_STILLNESS_MS) * 1000ULL) {
                    lowPowerMode = true;
                    lowPowerMotionWake.reset();
                    wifiTransport.setLowPowerMode(true, nowMs);
                    Serial.println("Stillness >5s; entering low-power schedule.");
                }
            } else {
                stationarySinceUs = 0;
            }
        } else if (lowPowerMotionWake.shouldExit(nowUs, !stationary, sensors.classifyingMotion,
                           sensors.hasMotionEvent ? &sensors.motionEvent : nullptr)) {
            lowPowerMode = false;
            stationarySinceUs = 0;
            wifiTransport.setLowPowerMode(false, nowMs);
            Serial.println("Motion detected; normal mode restored.");
        }
    }

    // Motion changes never restart or extend the independent GPS schedule.
    if (gpsCycle.startIfDue(nowMs)) {
        gps.setEnabled(true);
        Serial.println("GPS periodic window started.");
    }
    gps.update();

    if (sensors.hasVoiceEvent) {
        ++voiceDetectionCount;
        Serial.printf("VOICE TRIGGER p2p=%u rms=%.1f rise=%.2f\n", sensors.voiceEvent.peakToPeak,
                      sensors.voiceEvent.rms, sensors.voiceEvent.riseRatio);
        wifiTransport.enqueueVoiceDetection(sensors.voiceEvent, voiceDetectionCount);
    }

    if (sensors.hasVibrationEvent) {
        ++vibrationDetectionCount;
        Serial.printf("VIB TRIGGER peak=%.1f jerk=%.1f sat=%s\n",
                      sensors.vibrationEvent.peakMagnitude, sensors.vibrationEvent.peakJerk,
                      sensors.vibrationEvent.saturated ? "yes" : "no");
        wifiTransport.enqueueVibrationDetection(sensors.vibrationEvent, vibrationDetectionCount);
    }

    if (sensors.hasMotionEvent) {
        if (sensors.motionEvent.direction == MotionDirection::Vertical) ++verticalMotionCount;
        wifiTransport.onMotionDirection(sensors.motionEvent.direction, nowMs);
        shotDetector.onMotionEvent(sensors.motionEvent);
    }
    const GpsFix fix = gps.currentFix();
    ShotEvent shot;
    if (shotDetector.consumeShot(shot, fix)) {
        printShot(shot);
        wifiTransport.enqueue(shot);
    }

    // 汇总最新传感器状态；WifiTransport 会在自己的周期内打包上报。
    WifiTelemetry telemetry;
    telemetry.micRaw = sensors.micRaw;
    telemetry.micPeakToPeak = sensors.micPeakToPeak;
    telemetry.micRms = sensors.micRms;
    telemetry.vibrationReady = sensors.vibrationReady;
    telemetry.vibrationX = sensors.vibrationX;
    telemetry.vibrationY = sensors.vibrationY;
    telemetry.vibrationZ = sensors.vibrationZ;
    telemetry.vibrationMagnitude = sensors.vibrationMagnitude;
    telemetry.vibrationJerk = sensors.vibrationJerk;
    telemetry.tiltValid = sensors.tiltValid;
    telemetry.rollDegrees = sensors.rollDegrees;
    telemetry.pitchDegrees = sensors.pitchDegrees;
    telemetry.voiceFlag = sensors.hasVoiceEvent;
    telemetry.vibrationFlag = sensors.hasVibrationEvent;
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
    telemetry.verticalMotionCount = verticalMotionCount;
    telemetry.voiceDroppedSamples = sensors.voiceDroppedSamples;
    telemetry.vibrationDroppedSamples = sensors.vibrationDroppedSamples;
    wifiTransport.setTelemetry(telemetry);

    if (!config::CAPACITY_TEST_MODE && gpsCycle.active() && fix.valid && !gpsSnapshotPending) {
        // Copy values under the sensor lock without consuming any new events.
        portENTER_CRITICAL(&sensorStateMux);
        telemetry.micRaw = voiceDetection.latestRaw();
        telemetry.micPeakToPeak = voiceDetection.latestPeakToPeak();
        telemetry.micRms = voiceDetection.latestRms();
        telemetry.vibrationX = vibrateDetection.rawX();
        telemetry.vibrationY = vibrateDetection.rawY();
        telemetry.vibrationZ = vibrateDetection.rawZ();
        portEXIT_CRITICAL(&sensorStateMux);
        gpsSnapshot = wifiTransport.makeTelemetryRecord(telemetry);
        gpsSnapshotPending = true;
        lastGpsFlashAttemptMs = nowMs - config::GPS_FLASH_RETRY_MS;
    }
    if (gpsSnapshotPending && nowMs - lastGpsFlashAttemptMs >= config::GPS_FLASH_RETRY_MS) {
        lastGpsFlashAttemptMs = nowMs;
        if (wifiTransport.appendTelemetrySnapshot(gpsSnapshot)) {
            gpsSnapshotPending = false;
            gps.setEnabled(false);
            gpsCycle.finish();
            Serial.println("GPS fix and sensor snapshot saved to Flash; GPS off.");
        } else {
            Serial.println("GPS snapshot Flash write failed; retaining original snapshot for retry.");
        }
    }
    if (gpsCycle.timedOut(nowMs)) {
        gps.setEnabled(false);
        gpsCycle.finish();
        Serial.println(gpsSnapshotPending
            ? "GPS 3-minute window ended; snapshot still pending Flash retry; GPS off."
            : "GPS timed out after 3 minutes without a saved fix; GPS off.");
    }
    wifiTransport.update(nowMs);


    // 定期输出健康状态和丢样/发送计数，便于现场排查。
    if (nowMs - lastDiagnosticMs >= config::DIAGNOSTIC_INTERVAL_MS) {
        lastDiagnosticMs = nowMs;
        Serial.printf(
            "mic[raw=%u p2p=%u rms=%.1f] mma[%s x=%u y=%u z=%u mag=%.1f jerk=%.1f] "
            "flags[%d,%d] power[%s] gps[%s sats=%lu chars=%lu] wifi[%s] "
            "heap[free=%lu min=%lu] adc[overrun=%lu] spool[written=%lu rate=%lu/s] "
            "udp[q=%u shot=%lu status=%lu fail=%lu drop=%lu] "
            "dropped[%lu,%lu]\n",
            sensors.micRaw, sensors.micPeakToPeak,
            sensors.micRms, sensors.vibrationReady ? "ready" : "cal",
            sensors.vibrationX, sensors.vibrationY, sensors.vibrationZ,
            sensors.vibrationMagnitude, sensors.vibrationJerk, sensors.hasVoiceEvent,
            sensors.hasVibrationEvent, lowPowerMode ? "low" : "normal", fix.valid ? "fix" : "search",
            static_cast<unsigned long>(fix.satellites),
            static_cast<unsigned long>(gps.parsedCharacters()),
            wifiTransport.connected() ? "connected" :
                (wifiTransport.radioEnabled() ? "search" : "off"),
            static_cast<unsigned long>(ESP.getFreeHeap()),
            static_cast<unsigned long>(ESP.getMinFreeHeap()),
            static_cast<unsigned long>(adcSampler.overrunCount()),
            static_cast<unsigned long>(wifiTransport.telemetryWritesTotal()),
            static_cast<unsigned long>(wifiTransport.telemetryWritesPerSecond()),
            wifiTransport.queuedEvents(),
            static_cast<unsigned long>(wifiTransport.sentEvents()),
            static_cast<unsigned long>(wifiTransport.sentStatuses()),
            static_cast<unsigned long>(wifiTransport.failedSends()),
            static_cast<unsigned long>(wifiTransport.droppedEvents()),
            static_cast<unsigned long>(sensors.voiceDroppedSamples),
            static_cast<unsigned long>(sensors.vibrationDroppedSamples));
    }

    // Block only this loop task, never the independent 1 kHz DMA reader.
    // Retain the polling fallback's timing if the DMA worker did not start.
    if (adcReaderTaskStarted && adcSampler.ready()) {
        vTaskDelay(1);
    } else {
        yield();
    }
}
