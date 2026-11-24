#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stddef.h>

// Fixed-size flash record. Sequence and checksum are transport metadata.
struct __attribute__((packed)) TelemetryRecord {
    uint32_t magic = 0x544C4D32;  // TLM2 (reader also accepts TLM1)
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
    uint8_t recordKind = 0; // 0: legacy/combined snapshot, 1: sensors, 2: GPS track.
    uint8_t reserved[2] = {}; // [0]=1: accel slots hold offset centidegrees roll/pitch and validity.
    uint64_t bootId = 0;
    uint64_t uptimeMs = 0;
    uint32_t shotId = 0;  // Nonzero: durable shot location, not a sensor sample.
    uint32_t checksum = 0;
};
static_assert(sizeof(TelemetryRecord) == 90 && offsetof(TelemetryRecord, bootId) == 66,
              "TLM2 Flash layout must remain compatible with existing records");

class TelemetrySpool {
public:
#ifdef DEVICE_ARCHIVE_UPLOAD
    uint64_t ordinaryThrough() const { return ordinaryThrough_; }
    uint64_t gpsThrough() const { return gpsThrough_; }
    bool confirmOrdinary(uint64_t through);
    bool confirmGps(uint64_t through);
    bool readOrdinary(uint64_t after, uint64_t through, TelemetryRecord* rows,
                      size_t capacity, size_t& count, uint64_t& scanned);
    bool readLiveGps(uint64_t after, uint64_t through, TelemetryRecord* rows,
                     size_t capacity, size_t& count, uint64_t& scanned);
    bool readShots(uint64_t after, uint64_t through, TelemetryRecord* rows,
                   size_t capacity, size_t& count, uint64_t& scanned);
    bool reclaimAcknowledged() { return compactAcknowledgedSegments(); }
    size_t peekArchiveBatch(TelemetryRecord* rows, size_t capacity);
    bool acknowledgeArchiveBatch(const TelemetryRecord* rows, size_t count);
    static bool ordinary(const TelemetryRecord& r) {
        return r.recordKind == 1 && !r.shotId;
    }
    static bool liveGps(const TelemetryRecord& r) {
        return r.recordKind == 2 && !r.shotId && r.gpsValid;
    }
#endif
    bool begin(bool capacityTestMode = false);
    bool append(const TelemetryRecord& sample);
    size_t appendBatch(const TelemetryRecord* samples, size_t count);
    bool peek(TelemetryRecord& sample);
    size_t peekBatch(TelemetryRecord* records, size_t capacity);
    bool acknowledge(uint64_t sequence);
    bool acknowledgeBatch(const uint64_t* sequences, size_t count);
    void latchCapacityTestFull();
    uint64_t pendingCount() const;
    uint64_t capacityTestCurrentRecordCount() const { return capacityTestCurrentRecordCount_; }
    bool capacityTestMode() const { return capacityTestMode_; }
    bool capacityTestFull() const { return capacityTestFull_; }
    bool capacityTestResultSaved() const { return capacityTestResultSaved_; }
    uint64_t capacityTestRecordCount() const { return capacityTestRecordCount_; }
    uint32_t capacityTestUsedBytes() const { return capacityTestUsedBytes_; }
    uint32_t capacityTestTotalBytes() const { return capacityTestTotalBytes_; }
    uint32_t storageUsedBytes() const;
    uint32_t storageTotalBytes() const;
    bool storageFullSnapshotValid() const { return storageFullSnapshotValid_; }
    uint64_t storageFullSnapshotRecords() const { return storageFullSnapshotRecords_; }
    bool full() const { return full_; }
    uint64_t lastSequence() const { return nextSequence_ ? nextSequence_ - 1 : 0; }

private:
#ifdef DEVICE_ARCHIVE_UPLOAD
    uint64_t ordinaryThrough_ = 0, gpsThrough_ = 0;
    uint64_t segmentLast_[512] = {};
    uint64_t segmentLastShot_[512] = {};
    static bool shotRecord(const TelemetryRecord& row) { return row.shotId != 0; }
    bool batchReclaimLog_ = false;
    uint32_t batchReclaimedFiles_ = 0;
    uint64_t batchReclaimedBytes_ = 0;
    size_t walkArchiveBatch(TelemetryRecord* output, const TelemetryRecord* expected, size_t capacity);
    bool readSelected(uint64_t after, uint64_t through, TelemetryRecord* rows,
                      size_t capacity, size_t& count, uint64_t& scanned,
                      bool (*include)(const TelemetryRecord&));
#endif
    static constexpr size_t SEGMENT_BYTES = 32768;
    static constexpr uint32_t LITTLEFS_WRITE_RESERVE_BYTES = 8192;
    static constexpr uint32_t RECORD_MAGIC = 0x544C4D32;
    // 512 x 32 KiB segments can address 16 MiB of queued telemetry files.
    static constexpr uint32_t SEGMENT_LIMIT = 512;
    static uint32_t checksum(const TelemetryRecord& record);
    static bool valid(const TelemetryRecord& record);
    static bool readRecord(fs::File& file, TelemetryRecord& record);
    static void segmentPath(uint32_t index, char* path, size_t size);
    static bool probeSegment(uint32_t index, bool& present);
    bool scan();
    bool reserveSequenceBlock();
    bool compactAcknowledgedSegments();
    void latchStorageFullSnapshot();
    void clearStorageFullSnapshotIfSpaceAvailable();
    bool findPending(TelemetryRecord& sample, uint32_t* segment, size_t* offset);
    bool advanceSegment(uint32_t segment, size_t* exhaustedBytes = nullptr);

    uint32_t writeSegment_ = 0; // Newest record's segment; not the highest filename.
    uint64_t nextSequence_ = 1;
    uint64_t reservedSequenceEnd_ = 1;
    uint64_t acknowledgedSequence_ = 0;
    uint64_t persistedAcknowledgedSequence_ = 0;
    uint64_t capacityTestRecordCount_ = 0;
    uint64_t capacityTestCurrentRecordCount_ = 0;
    uint64_t pendingRecordCount_ = 0;
    uint32_t capacityTestUsedBytes_ = 0;
    uint32_t capacityTestTotalBytes_ = 0;
    uint64_t storageFullSnapshotRecords_ = 0;
    uint32_t storageFullSnapshotUsedBytes_ = 0;
    uint32_t storageFullSnapshotTotalBytes_ = 0;
    TelemetryRecord segmentHeads_[SEGMENT_LIMIT] = {};
    size_t segmentOffsets_[SEGMENT_LIMIT] = {};
    size_t segmentRecordBytes_[SEGMENT_LIMIT] = {};
    bool segmentHasPending_[SEGMENT_LIMIT] = {};
    bool segmentAppendable_[SEGMENT_LIMIT] = {};
    bool segmentPresent_[SEGMENT_LIMIT] = {}; // Owner-maintained index; no periodic disk probes.
    bool mounted_ = false;
    bool full_ = false;
    bool capacityTestMode_ = false;
    bool capacityTestFull_ = false;
    bool capacityTestResultSaved_ = false;
    bool storageFullSnapshotValid_ = false;
};
