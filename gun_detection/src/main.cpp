#include <Arduino.h>
#include <atomic>
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "adc_sampler.h"
#include "gps_manager.h"
#include "gps_cycle.h"
#include "daily_standby.h"
#include "standby_test.h"
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
bool gpsWasAlwaysOn = false;
bool gpsSnapshotPending = false;
TelemetryRecord gpsSnapshot;
uint32_t lastGpsFlashAttemptMs = 0;
const DailyStandby dailyStandby(config::NIGHT_STANDBY_START_MINUTE,
                               config::NIGHT_STANDBY_END_MINUTE);
bool nightStandbyActive = false;
bool nightStandbyReady = false;
StandbyTest quickStandby(static_cast<uint64_t>(config::STANDBY_TEST_DELAY_MINUTES) * 60000000ULL,
                        static_cast<uint64_t>(config::STANDBY_TEST_SLEEP_MINUTES) * 60000000ULL);
std::atomic<uint32_t> testSensorFrames{0};
uint64_t testResumeCheckUs = 0, testResumeStartUs = 0;
uint32_t testResumeStartFrames = 0;
struct StandbyAudit {
    int64_t rtcUs = 0;
    uint64_t uptimeUs = 0, storedRecords = 0;
    uint32_t frames = 0, writes = 0, shots = 0;
    uint8_t ramRecords = 0, events = 0;
    bool gpsPending = false;
} standbyAudit;


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
        if constexpr (config::STANDBY_TEST_ENABLED) ++testSensorFrames;
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
            // Acknowledge only after DMA is actually stopped by its owner task.
            if (!adcSampler.stop()) {
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
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

static int64_t systemClockUs() {
    timeval value;
    gettimeofday(&value, nullptr);
    return static_cast<int64_t>(value.tv_sec) * 1000000 + value.tv_usec;
}

static void printTestClock(const char* label) {
    const time_t utc = time(nullptr);
    if (static_cast<int64_t>(utc) < gps_clock::MIN_VALID_EPOCH) {
        Serial.printf("%s calendar=UNSYNCED; GPS/NTP sync required to verify calendar continuity.\n", label);
        return;
    }
    const time_t local = utc + 8 * 3600;
    tm parts = {};
    char stamp[24] = {};
    gmtime_r(&local, &parts);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &parts);
    Serial.printf("%s Beijing=%s\n", label, stamp);
}

static void captureStandbyAudit() {
    if (!config::STANDBY_TEST_ENABLED) return;
    standbyAudit.rtcUs = systemClockUs();
    standbyAudit.uptimeUs = esp_timer_get_time();
    standbyAudit.frames = testSensorFrames.load();
    standbyAudit.writes = wifiTransport.telemetryWritesTotal();
    standbyAudit.storedRecords = wifiTransport.pendingStoredRecords();
    standbyAudit.ramRecords = wifiTransport.pendingRamRecords();
    standbyAudit.events = wifiTransport.queuedEvents();
    standbyAudit.shots = shotDetector.shotCount();
    standbyAudit.gpsPending = gpsSnapshotPending;
    printTestClock("STANDBY_BEFORE");
    Serial.printf("STANDBY_BEFORE uptime_ms=%llu flash_pending=%llu ram_pending=%u events=%u shots=%lu frames=%lu\n",
                  static_cast<unsigned long long>(standbyAudit.uptimeUs / 1000),
                  static_cast<unsigned long long>(standbyAudit.storedRecords),
                  standbyAudit.ramRecords, standbyAudit.events,
                  static_cast<unsigned long>(standbyAudit.shots), static_cast<unsigned long>(standbyAudit.frames));
}

static void reportStandbyAudit() {
    if (!config::STANDBY_TEST_ENABLED || !nightStandbyReady) return;
    const uint64_t nowUs = esp_timer_get_time();
    const int64_t rtcDelta = systemClockUs() - standbyAudit.rtcUs;
    const bool pendingSame = standbyAudit.storedRecords == wifiTransport.pendingStoredRecords() &&
        standbyAudit.ramRecords == wifiTransport.pendingRamRecords() &&
        standbyAudit.events == wifiTransport.queuedEvents() && standbyAudit.gpsPending == gpsSnapshotPending;
    printTestClock("STANDBY_AFTER");
    Serial.printf("STANDBY_AUDIT rtc_advance_ms=%lld uptime_advance_ms=%llu adc_frames_delta=%lu flash_writes_delta=%lu pending_counts=%s shot_count=%s\n",
                  static_cast<long long>(rtcDelta / 1000),
                  static_cast<unsigned long long>((nowUs - standbyAudit.uptimeUs) / 1000),
                  static_cast<unsigned long>(testSensorFrames.load() - standbyAudit.frames),
                  static_cast<unsigned long>(wifiTransport.telemetryWritesTotal() - standbyAudit.writes),
                  pendingSame ? "UNCHANGED" : "CHANGED",
                  standbyAudit.shots == shotDetector.shotCount() ? "UNCHANGED" : "CHANGED");
    testResumeStartUs = nowUs;
    testResumeStartFrames = testSensorFrames.load();
    testResumeCheckUs = nowUs + 3000000ULL;
}

static bool serviceNightStandby(uint32_t nowMs) {
    const bool requested = !config::CAPACITY_TEST_MODE && (config::STANDBY_TEST_ENABLED
        ? quickStandby.requested(static_cast<uint64_t>(esp_timer_get_time()))
        : config::NIGHT_STANDBY_ENABLED && dailyStandby.contains(static_cast<int64_t>(time(nullptr))));
    if (!requested) {
        if (nightStandbyActive) {
            // Audit before sensors/Wi-Fi resume, so they cannot change the counts or RTC yet.
            reportStandbyAudit();
            if (config::STANDBY_TEST_ENABLED) {
                quickStandby.finish();
                Serial.println("STANDBY_TEST_COMPLETE: one-shot finished; daily standby disabled in this test build.");
            }
            gpio_hold_dis(static_cast<gpio_num_t>(config::MMA_SLEEP_PIN));
            gpio_hold_dis(static_cast<gpio_num_t>(config::GPS_ENABLE_PIN));
            digitalWrite(config::MMA_SLEEP_PIN, HIGH);
            lowPowerMode = false;
            stationarySinceUs = 0;
            lowPowerMotionWake.reset();
            wifiTransport.setLowPowerMode(false, nowMs);
            wifiTransport.setScheduledStandby(false, nowMs);
            gps.setEnabled(true);
            gpsCycle.startNow(nowMs);
            gpsWasAlwaysOn = false;
            nightStandbyActive = nightStandbyReady = false;
            // The normal resume path below releases pauseSampling and resets
            // detection calibration, including the no-worker fallback path.
            Serial.println("NIGHT_STANDBY_EXIT: morning work and sensor calibration resuming.");
        }
        return false;
    }

    if (!nightStandbyActive) {
        nightStandbyActive = true;
        pauseSampling.store(true);
        gps.setEnabled(false);
        gpsCycle.finish();
        gpsWasAlwaysOn = false;
        wifiTransport.setScheduledStandby(true, nowMs);
        shotDetector.clearPending();
        Serial.println("NIGHT_STANDBY_ENTER: stopping sampling, GPS, Wi-Fi and periodic recording.");
    }

    // Never enter sleep while the independent ADC task is still sampling.
    if (adcReaderTaskStarted && !samplingPaused.load() &&
        (adcSampler.ready() || adcSampler.running())) {
        delay(1);
        return true;
    }
    if (!adcReaderTaskStarted && !adcSampler.stop()) {
        delay(5);
        return true;
    }

    if (!nightStandbyReady) {
        digitalWrite(config::MMA_SLEEP_PIN, LOW);
        if (!wifiTransport.flushPendingTelemetry()) {
            Serial.println("Night standby: uncommitted telemetry retained in RAM; Flash unavailable.");
        }
        if (gpsSnapshotPending && wifiTransport.appendTelemetrySnapshot(gpsSnapshot)) {
            gpsSnapshotPending = false;
        }
        if (gpsSnapshotPending) {
            Serial.println("Night standby: uncommitted GPS snapshot retained in RAM.");
        }
        // Hold both peripheral enable pins LOW throughout light sleep.
        gpio_hold_en(static_cast<gpio_num_t>(config::MMA_SLEEP_PIN));
        gpio_hold_en(static_cast<gpio_num_t>(config::GPS_ENABLE_PIN));
        nightStandbyReady = true;
        if (config::STANDBY_TEST_ENABLED) {
            captureStandbyAudit();
            quickStandby.beginSleep(static_cast<uint64_t>(esp_timer_get_time()));
            Serial.printf("STANDBY_TEST_SLEEP: %u minutes; waiting for timer wake.\n", config::STANDBY_TEST_SLEEP_MINUTES);
        } else {
            Serial.printf("Night standby: RTC and RAM retained; timer wake at %02u:%02u Beijing time.\n",
                          config::NIGHT_STANDBY_END_MINUTE / 60, config::NIGHT_STANDBY_END_MINUTE % 60);
        }
    }

    // Re-read the clock after shutdown/Flash work; a final NTP callback may
    // have corrected it. No sampling, storage or network loop runs in sleep.
    const uint64_t remainingUs = config::STANDBY_TEST_ENABLED
        ? quickStandby.remainingUs(static_cast<uint64_t>(esp_timer_get_time()))
        : static_cast<uint64_t>(dailyStandby.secondsUntilEnd(static_cast<int64_t>(time(nullptr)))) * 1000000ULL;
    if (remainingUs == 0) return true;
    Serial.flush();
    esp_err_t error = esp_sleep_enable_timer_wakeup(remainingUs);
    if (error == ESP_OK) error = esp_light_sleep_start();
    if (error != ESP_OK) {
        // Keep peripherals stopped, and avoid busy retrying on sleep rejection.
        if (nowMs - lastDiagnosticMs >= config::DIAGNOSTIC_INTERVAL_MS) {
            lastDiagnosticMs = nowMs;
            Serial.printf("Night light-sleep failed: %s; peripherals remain stopped.\n", esp_err_to_name(error));
        }
        delay(1000);
    }
    return true;  // Re-evaluate time with fresh millis()/RTC on the next loop.
}

void setup() {
    Serial.begin(config::SERIAL_BAUD);
    delay(500);
    Serial.println();
    Serial.println("Gun detection firmware starting...");
    Serial.printf("CPU frequency: %lu MHz\n", static_cast<unsigned long>(getCpuFrequencyMhz()));
    Serial.printf("Daily standby: %s, Beijing %02u:%02u-%02u:%02u; valid calendar required.\n",
                  config::NIGHT_STANDBY_ENABLED && !config::CAPACITY_TEST_MODE && !config::STANDBY_TEST_ENABLED ? "enabled" : "disabled",
                  config::NIGHT_STANDBY_START_MINUTE / 60, config::NIGHT_STANDBY_START_MINUTE % 60,
                  config::NIGHT_STANDBY_END_MINUTE / 60, config::NIGHT_STANDBY_END_MINUTE % 60);

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

    if (config::STANDBY_TEST_ENABLED) {
        quickStandby.arm(static_cast<uint64_t>(esp_timer_get_time()));
        Serial.printf("STANDBY_TEST_ARMED: work %u minutes, sleep %u minutes, then resume once; no data erase.\n",
                      config::STANDBY_TEST_DELAY_MINUTES, config::STANDBY_TEST_SLEEP_MINUTES);
    }
    Serial.println("MMA7361 and GPS enabled; continuous detection started.");
}

void loop() {
    // 微秒时钟驱动采样和动作识别，毫秒时钟用于联网周期任务。
    const uint64_t nowUs = esp_timer_get_time();
    const uint32_t nowMs = millis();
    if (serviceNightStandby(nowMs)) return;
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
        gpsWasAlwaysOn = false;
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
    if (config::STANDBY_TEST_ENABLED && testResumeCheckUs != 0 && nowUs >= testResumeCheckUs) {
        const uint32_t frames = testSensorFrames.load() - testResumeStartFrames;
        const uint64_t elapsedUs = nowUs - testResumeStartUs;
        Serial.printf("STANDBY_RESUME_CHECK frames=%lu per_channel_hz=%lu mma_calibrated=%s adc_overrun=%lu dropped=%lu,%lu; verify voice/motion triggers manually.\n",
                      static_cast<unsigned long>(frames),
                      static_cast<unsigned long>(static_cast<uint64_t>(frames) * 1000000ULL / elapsedUs),
                      sensors.vibrationReady ? "yes" : "no",
                      static_cast<unsigned long>(adcSampler.overrunCount()),
                      static_cast<unsigned long>(sensors.voiceDroppedSamples),
                      static_cast<unsigned long>(sensors.vibrationDroppedSamples));
        testResumeCheckUs = 0;
    }

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

    const bool gpsAlwaysOn = config::NORMAL_GPS_ALWAYS_ON && !lowPowerMode;
    if (gpsAlwaysOn != gpsWasAlwaysOn) {
        // Returning to low power gets a full acquisition window, regardless
        // of how long GPS was continuously enabled in normal mode.
        gpsCycle.startNow(nowMs);
        gps.setEnabled(true);
        Serial.println(gpsAlwaysOn ? "Normal GPS always-on mode enabled."
                                   : "GPS returning to periodic mode; new window started.");
    } else if (gpsCycle.startIfDue(nowMs)) {
        gps.setEnabled(true);
        Serial.println("GPS periodic window started.");
    }
    gpsWasAlwaysOn = gpsAlwaysOn;
    if (gpsAlwaysOn) gps.setEnabled(true);
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
            if (!gpsAlwaysOn) gps.setEnabled(false);
            gpsCycle.finish();
            Serial.println(gpsAlwaysOn
                ? "GPS fix and sensor snapshot saved to Flash; normal GPS remains on."
                : "GPS fix and sensor snapshot saved to Flash; GPS off.");
        } else {
            Serial.println("GPS snapshot Flash write failed; retaining original snapshot for retry.");
        }
    }
    if (gpsCycle.timedOut(nowMs)) {
        if (!gpsAlwaysOn) gps.setEnabled(false);
        gpsCycle.finish();
        if (gpsAlwaysOn) {
            Serial.println("GPS snapshot window ended; normal GPS remains on.");
        } else {
            Serial.println(gpsSnapshotPending
                ? "GPS 3-minute window ended; snapshot still pending Flash retry; GPS off."
                : "GPS timed out after 3 minutes without a saved fix; GPS off.");
        }
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
