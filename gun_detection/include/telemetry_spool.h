#pragma once

#include <Arduino.h>

// Fixed-size flash record. Sequence and checksum are transport metadata.
struct __attribute__((packed)) TelemetryRecord {
    uint32_t magic = 0x544C4D31;  // TLM1
    uint64_t sequence = 0;
    char timestamp[20] = {};
    uint16_t accelX = 0;
    uint16_t accelY = 0;
    uint16_t accelZ = 0;
    uint16_t micRaw = 0;
    uint16_t micP2p = 0;
    float micRms = 0.0F;
    double latitude = 0.0;
    double longitude = 0.0;
    uint8_t gpsValid = 0;
    uint8_t reserved[3] = {};
    uint32_t checksum = 0;
};

class TelemetrySpool {
public:
    bool begin();
    bool append(const TelemetryRecord& sample);
    bool peek(TelemetryRecord& sample);
    size_t peekBatch(TelemetryRecord* records, size_t capacity);
    bool acknowledge(uint64_t sequence);
    bool full() const { return full_; }
    uint64_t lastSequence() const { return nextSequence_ ? nextSequence_ - 1 : 0; }

private:
    static constexpr size_t SEGMENT_BYTES = 32768;
    static constexpr uint32_t RECORD_MAGIC = 0x544C4D31;
    static constexpr uint32_t SEGMENT_LIMIT = 256;
    static uint32_t checksum(const TelemetryRecord& record);
    static bool valid(const TelemetryRecord& record);
    static void segmentPath(uint32_t index, char* path, size_t size);
    bool scan();
    bool reserveSequenceBlock();
    bool compactAcknowledgedSegments();
    bool findPending(TelemetryRecord& sample, uint32_t* segment, size_t* offset);
    void advanceSegment(uint32_t segment);

    uint32_t latestSegment_ = 0;
    uint64_t nextSequence_ = 1;
    uint64_t reservedSequenceEnd_ = 1;
    uint64_t acknowledgedSequence_ = 0;
    uint64_t persistedAcknowledgedSequence_ = 0;
    TelemetryRecord segmentHeads_[SEGMENT_LIMIT] = {};
    size_t segmentOffsets_[SEGMENT_LIMIT] = {};
    bool segmentHasPending_[SEGMENT_LIMIT] = {};
    bool mounted_ = false;
    bool full_ = false;
};
