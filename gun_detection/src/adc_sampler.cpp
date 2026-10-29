#include "adc_sampler.h"

#include <driver/adc.h>
#include <esp_err.h>
#include <soc/soc_caps.h>

#include "system_config.h"

namespace {
constexpr uint32_t ADC_CHANNEL_RATE_HZ =
    config::VOICE_SAMPLE_RATE_HZ > config::VIBRATE_SAMPLE_RATE_HZ
        ? config::VOICE_SAMPLE_RATE_HZ
        : config::VIBRATE_SAMPLE_RATE_HZ;
constexpr uint32_t ADC_CONVERSION_RATE_HZ = ADC_CHANNEL_RATE_HZ * 4U;
constexpr uint32_t ADC_MAX_STORE_BYTES = 16384;
constexpr uint32_t ADC_INTERRUPT_BYTES = 256;
constexpr size_t ADC_CONVERSION_BYTES = sizeof(adc_digi_output_data_t);

uint8_t channelForPin(uint8_t pin) {
    switch (pin) {
        case 4: return ADC1_CHANNEL_3;
        case 5: return ADC1_CHANNEL_4;
        case 6: return ADC1_CHANNEL_5;
        case 8: return ADC1_CHANNEL_7;
        default: return ADC_CHANNEL_MAX;
    }
}

bool addPattern(adc_digi_pattern_config_t& pattern, uint8_t pin) {
    const uint8_t channel = channelForPin(pin);
    if (channel >= ADC1_CHANNEL_MAX) return false;
    pattern.atten = ADC_ATTEN_DB_12;
    pattern.channel = channel;
    // The IDF 4.4 legacy digital-DMA driver uses the zero-based ADC_NUM_1
    // value (0) in this field, while adc_unit_t::ADC_UNIT_1 is 1. Passing the
    // latter makes the driver treat this as ADC2 and reject DMA on ESP32-S3.
    pattern.unit = 0;
    // In the IDF 4.4 legacy DMA API, this field is checked against the
    // bit-count SOC_ADC_DIGI_MAX_BITWIDTH (12), not ADC_WIDTH_BIT_12's enum
    // encoding (3, which is used by one-shot ADC configuration).
    pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
    return true;
}
}

bool AdcSampler::begin() {
    // Keep all four inputs in the same ADC1 scan; do not mix Arduino analogRead
    // one-shot conversions with the continuous digital controller.
    adc_digi_pattern_config_t pattern[4] = {};
    if (!addPattern(pattern[0], config::MIC_ADC_PIN) ||
        !addPattern(pattern[1], config::MMA_X_PIN) ||
        !addPattern(pattern[2], config::MMA_Y_PIN) ||
        !addPattern(pattern[3], config::MMA_Z_PIN)) {
        Serial.println("ADC DMA: configured pin is not an ESP32-S3 ADC1 input.");
        return false;
    }

    const uint32_t channelMask = (1UL << pattern[0].channel) |
                                 (1UL << pattern[1].channel) |
                                 (1UL << pattern[2].channel) |
                                 (1UL << pattern[3].channel);
    adc_digi_init_config_t init = {};
    init.max_store_buf_size = ADC_MAX_STORE_BYTES;
    init.conv_num_each_intr = ADC_INTERRUPT_BYTES;
    init.adc1_chan_mask = channelMask;
    init.adc2_chan_mask = 0;
    esp_err_t error = adc_digi_initialize(&init);
    if (error != ESP_OK) {
        Serial.printf("ADC DMA: initialize failed (%s).\n", esp_err_to_name(error));
        return false;
    }

    adc_digi_configuration_t controller = {};
    controller.conv_limit_en = false;
    controller.conv_limit_num = 0;
    controller.pattern_num = 4;
    controller.adc_pattern = pattern;
    controller.sample_freq_hz = ADC_CONVERSION_RATE_HZ;
    controller.conv_mode = ADC_CONV_SINGLE_UNIT_1;
    controller.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    error = adc_digi_controller_configure(&controller);
    if (error != ESP_OK) {
        Serial.printf("ADC DMA: controller configuration failed (%s).\n", esp_err_to_name(error));
        adc_digi_deinitialize();
        return false;
    }

    ready_ = true;
    return true;
}

bool AdcSampler::start() {
    if (!ready_) return false;
    if (running_) return true;
    const esp_err_t error = adc_digi_start();
    if (error != ESP_OK) {
        Serial.printf("ADC DMA: start failed (%s).\n", esp_err_to_name(error));
        ready_ = false;
        return false;
    }
    running_ = true;
    Serial.printf("ADC DMA ready: %lu conversions/s, 1 kHz per input.\n",
                  static_cast<unsigned long>(ADC_CONVERSION_RATE_HZ));
    return true;
}

size_t AdcSampler::read(AdcSample* samples, size_t capacity, uint64_t newestSampleUs) {
    if (!ready_ || !running_ || samples == nullptr || capacity == 0) return 0;

    uint32_t bytesRead = 0;
    const esp_err_t error = adc_digi_read_bytes(dmaBytes_, sizeof(dmaBytes_), &bytesRead, 0);
    if (error == ESP_ERR_INVALID_STATE) {
        ++overrunCount_;
        lastSampleTimestampUs_ = 0;
        // The legacy IDF ADC lock is task-owned: start, stop and restart must
        // all run in the same task. read() is called only by adcReaderTask.
        const esp_err_t stopError = adc_digi_stop();
        if (stopError != ESP_OK) {
            Serial.printf("ADC DMA: overrun stop failed (%s).\n", esp_err_to_name(stopError));
            ready_ = false;
            running_ = false;
            return 0;
        }
        running_ = false;
        if (!start()) return 0;
        return 0;
    }
    if (error != ESP_OK || bytesRead < ADC_CONVERSION_BYTES) return 0;

    const size_t available = bytesRead / ADC_CONVERSION_BYTES;
    const size_t count = min(available, capacity);
    const uint64_t conversionIntervalUs = 1000000ULL / ADC_CONVERSION_RATE_HZ;
    const uint64_t spanUs = (count > 0 ? count - 1 : 0) * conversionIntervalUs;
    const uint64_t firstSampleUs = lastSampleTimestampUs_ != 0
        ? lastSampleTimestampUs_ + conversionIntervalUs
        : (newestSampleUs > spanUs ? newestSampleUs - spanUs : 0);
    size_t outputCount = 0;

    for (size_t i = 0; i < count; ++i) {
        const auto* result = reinterpret_cast<const adc_digi_output_data_t*>(
            dmaBytes_ + i * ADC_CONVERSION_BYTES);
        // The type2 DMA result stores a zero-based unit index (0 = ADC1,
        // 1 = ADC2), unlike adc_unit_t where ADC_UNIT_1 is 1.
        if (result->type2.unit != 0) continue;

        AdcInput input;
        switch (result->type2.channel) {
            case ADC1_CHANNEL_7: input = AdcInput::Microphone; break;
            case ADC1_CHANNEL_3: input = AdcInput::AccelX; break;
            case ADC1_CHANNEL_4: input = AdcInput::AccelY; break;
            case ADC1_CHANNEL_5: input = AdcInput::AccelZ; break;
            default: continue;
        }
        const uint64_t sampleTimeUs = firstSampleUs + i * conversionIntervalUs;
        samples[outputCount++] = {input, static_cast<uint16_t>(result->type2.data), sampleTimeUs};
    }
    if (count > 0) {
        lastSampleTimestampUs_ = firstSampleUs + (count - 1) * conversionIntervalUs;
    }
    return outputCount;
}

void AdcSampler::stop() {
    if (running_ && adc_digi_stop() == ESP_OK) {
        running_ = false;
        lastSampleTimestampUs_ = 0;
    }
}
