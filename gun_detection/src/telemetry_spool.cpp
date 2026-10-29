#include "telemetry_spool.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <esp_littlefs.h>
#include <esp_partition.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "system_config.h"

#ifndef GUN_CAPACITY_TEST_BUILD_ID
#define GUN_CAPACITY_TEST_BUILD_ID 0ULL
#endif

namespace {
Preferences preferences;
}

uint32_t TelemetrySpool::checksum(const TelemetryRecord& record) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < offsetof(TelemetryRecord, checksum); ++i) {
        value = (value ^ bytes[i]) * 16777619u;
    }
    return value;
}

bool TelemetrySpool::valid(const TelemetryRecord& record) {
    return record.magic == RECORD_MAGIC && record.checksum == checksum(record);
}

// Read TLM1 in place; never rewrite or discard legacy cached samples.
bool TelemetrySpool::readRecord(fs::File& file, TelemetryRecord& record) {
    record = TelemetryRecord{};
    if (file.read(reinterpret_cast<uint8_t*>(&record.magic), 4) != 4) return false;
    if (record.magic == RECORD_MAGIC) {
        return file.read(reinterpret_cast<uint8_t*>(&record) + 4, sizeof(record) - 4) ==
                   sizeof(record) - 4 && valid(record);
    }
    if (record.magic != 0x544C4D31) return false;
    constexpr size_t prefix = offsetof(TelemetryRecord, bootId);
    uint32_t oldChecksum = 0;
    if (file.read(reinterpret_cast<uint8_t*>(&record) + 4, prefix - 4) != prefix - 4 ||
        file.read(reinterpret_cast<uint8_t*>(&oldChecksum), 4) != 4) return false;
    uint32_t value = 2166136261u;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
    for (size_t i = 0; i < prefix; ++i) value = (value ^ bytes[i]) * 16777619u;
    if (value != oldChecksum) return false;
    record.magic = RECORD_MAGIC;
    record.checksum = checksum(record);
    return true;
}

uint32_t TelemetrySpool::storageUsedBytes() const {
    if (capacityTestMode_ && capacityTestFull_) return capacityTestUsedBytes_;
    if (storageFullSnapshotValid_) return storageFullSnapshotUsedBytes_;
    return mounted_ ? static_cast<uint32_t>(LittleFS.usedBytes()) : capacityTestUsedBytes_;
}

uint32_t TelemetrySpool::storageTotalBytes() const {
    if (capacityTestMode_ && capacityTestFull_) return capacityTestTotalBytes_;
    if (storageFullSnapshotValid_) return storageFullSnapshotTotalBytes_;
    return mounted_ ? static_cast<uint32_t>(LittleFS.totalBytes()) : capacityTestTotalBytes_;
}

void TelemetrySpool::latchStorageFullSnapshot() {
    if (capacityTestMode_ || storageFullSnapshotValid_ || !mounted_) return;
    storageFullSnapshotRecords_ = pendingCount();
    storageFullSnapshotUsedBytes_ = static_cast<uint32_t>(LittleFS.usedBytes());
    storageFullSnapshotTotalBytes_ = static_cast<uint32_t>(LittleFS.totalBytes());
    storageFullSnapshotValid_ = true;
    Serial.printf("Telemetry storage full snapshot: records=%llu used=%lu/%lu bytes\n",
                  static_cast<unsigned long long>(storageFullSnapshotRecords_),
                  static_cast<unsigned long>(storageFullSnapshotUsedBytes_),
                  static_cast<unsigned long>(storageFullSnapshotTotalBytes_));
}

void TelemetrySpool::clearStorageFullSnapshotIfSpaceAvailable() {
    if (capacityTestMode_ || !storageFullSnapshotValid_ || !mounted_) return;
    const uint32_t totalBytes = static_cast<uint32_t>(LittleFS.totalBytes());
    const uint32_t usedBytes = static_cast<uint32_t>(LittleFS.usedBytes());
    if (totalBytes > usedBytes && totalBytes - usedBytes > LITTLEFS_WRITE_RESERVE_BYTES) {
        storageFullSnapshotValid_ = false;
        storageFullSnapshotRecords_ = 0;
        storageFullSnapshotUsedBytes_ = 0;
        storageFullSnapshotTotalBytes_ = 0;
    }
}

void TelemetrySpool::segmentPath(uint32_t index, char* path, size_t size) {
    snprintf(path, size, "/q%08lu.bin", static_cast<unsigned long>(index));
}

bool TelemetrySpool::begin(bool capacityTestMode) {
    capacityTestMode_ = capacityTestMode;
    if (!preferences.begin("tlm-spool", false)) {
        Serial.println("Telemetry spool: NVS open failed.");
        return false;
    }

    const esp_partition_t* littleFsPartition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
    if (littleFsPartition == nullptr) {
        Serial.println("Telemetry spool: LittleFS partition 'spiffs' not found.");
        return false;
    }

    const bool firstInitialization = !preferences.getBool("fsinit", false);
    const uint32_t currentFsBytes = littleFsPartition->size;
    const uint32_t previousFsBytes = preferences.getUInt("fsbytes", 0);
    // LittleFS can grow an existing filesystem, but it cannot shrink it in place.
    // If the partition size changed, clear the old cache before mounting so the
    // filesystem is created with the new partition geometry instead of asserting
    // in lfs_fs_grow_(). The user chose to discard the pre-resize telemetry cache.
    if (!firstInitialization && previousFsBytes != currentFsBytes) {
        Serial.printf("LittleFS partition size changed (%lu -> %lu bytes); clearing old cache.\n",
                      static_cast<unsigned long>(previousFsBytes),
                      static_cast<unsigned long>(currentFsBytes));
        const esp_err_t formatResult = esp_littlefs_format("spiffs");
        if (formatResult != ESP_OK) {
            Serial.printf("Telemetry spool: LittleFS resize format failed: %s.\n",
                          esp_err_to_name(formatResult));
            return false;
        }
    }

    mounted_ = LittleFS.begin(firstInitialization);
    if (!mounted_) {
        Serial.println("Telemetry spool: LittleFS mount failed; flash was not formatted.");
        return false;
    }
    if ((firstInitialization && preferences.putBool("fsinit", true) == 0) ||
        preferences.putUInt("fsbytes", currentFsBytes) != sizeof(uint32_t)) {
        Serial.println("Telemetry spool: could not persist filesystem geometry marker.");
        return false;
    }
    acknowledgedSequence_ = preferences.getULong64("acked", 0);
    persistedAcknowledgedSequence_ = acknowledgedSequence_;
    if (!scan()) {
        Serial.println("Telemetry spool: scan failed.");
        return false;
    }
    const uint64_t previousReservedEnd = preferences.getULong64("seqend", 1);
    nextSequence_ = max(nextSequence_, previousReservedEnd);
    reservedSequenceEnd_ = previousReservedEnd;

    const uint64_t previousCapacityTestBuild = preferences.getULong64("ctbuild", 0);
    const uint64_t currentCapacityTestBuild = static_cast<uint64_t>(GUN_CAPACITY_TEST_BUILD_ID);
    const bool newCapacityTestBuild = currentCapacityTestBuild != 0 &&
                                      previousCapacityTestBuild != currentCapacityTestBuild;
    if (capacityTestMode_ &&
        (!preferences.getBool("ctinit", false) || newCapacityTestBuild)) {
        // A newly compiled test image starts a fresh run. Rebooting that same image
        // preserves progress and monotonic sequence numbers.
        for (uint32_t index = 0; index < SEGMENT_LIMIT; ++index) {
            char path[20];
            segmentPath(index, path, sizeof(path));
            if (LittleFS.exists(path) && !LittleFS.remove(path)) {
                Serial.printf("Capacity test: failed to remove old segment %lu.\n",
                              static_cast<unsigned long>(index));
                return false;
            }
        }
        acknowledgedSequence_ = nextSequence_ ? nextSequence_ - 1 : 0;
        if (preferences.putULong64("acked", acknowledgedSequence_) != sizeof(uint64_t) ||
            preferences.putBool("ctfull", false) == 0 ||
            preferences.putULong64("ctcount", 0) != sizeof(uint64_t) ||
            preferences.putUInt("ctused", 0) == 0 ||
            preferences.putUInt("cttotal", LittleFS.totalBytes()) == 0 ||
            preferences.putBool("ctinit", true) == 0 ||
            preferences.putULong64("ctbuild", currentCapacityTestBuild) != sizeof(uint64_t)) {
            Serial.println("Capacity test: failed to initialize test metadata.");
            return false;
        }
        persistedAcknowledgedSequence_ = acknowledgedSequence_;
        if (!scan()) {
            Serial.println("Capacity test: scan failed after clearing old data.");
            return false;
        }
        reservedSequenceEnd_ = previousReservedEnd;
        Serial.println("Capacity test: old LittleFS telemetry cache cleared once.");
    }

    capacityTestFull_ = capacityTestMode_ && preferences.getBool("ctfull", false);
    capacityTestResultSaved_ = capacityTestFull_;
    capacityTestRecordCount_ = capacityTestMode_ ? preferences.getULong64("ctcount", 0) : 0;
    capacityTestCurrentRecordCount_ = capacityTestMode_
        ? (capacityTestFull_ ? capacityTestRecordCount_ : pendingCount())
        : 0;
    capacityTestUsedBytes_ = capacityTestMode_ ? preferences.getUInt("ctused", 0) : 0;
    capacityTestTotalBytes_ = capacityTestMode_ ? preferences.getUInt("cttotal", LittleFS.totalBytes()) : 0;
    if (capacityTestFull_) full_ = true;
    if (capacityTestMode_) {
        if (capacityTestFull_) {
            Serial.printf("CAPACITY_TEST_RESULT records=%llu used=%lu/%lu bytes\n",
                          static_cast<unsigned long long>(capacityTestRecordCount_),
                          static_cast<unsigned long>(capacityTestUsedBytes_),
                          static_cast<unsigned long>(capacityTestTotalBytes_));
        } else {
            Serial.println("CAPACITY_TEST_ACTIVE: telemetry uploads will wait until storage is full.");
        }
    }

    if (!reserveSequenceBlock()) {
        Serial.println("Telemetry spool: cannot reserve unique sequence numbers.");
        return false;
    }
    compactAcknowledgedSegments();
    Serial.printf("Telemetry spool ready: next=%llu acked=%llu used=%lu/%lu bytes\n",
                  static_cast<unsigned long long>(nextSequence_),
                  static_cast<unsigned long long>(acknowledgedSequence_),
                  static_cast<unsigned long>(LittleFS.usedBytes()),
                  static_cast<unsigned long>(LittleFS.totalBytes()));
    return true;
}

bool TelemetrySpool::reserveSequenceBlock() {
    constexpr uint64_t SEQUENCE_BLOCK_SIZE = 1024;
    const uint64_t newEnd = nextSequence_ + SEQUENCE_BLOCK_SIZE;
    if (preferences.putULong64("seqend", newEnd) != sizeof(uint64_t)) return false;
    reservedSequenceEnd_ = newEnd;
    return true;
}

bool TelemetrySpool::scan() {
    memset(segmentHeads_, 0, sizeof(segmentHeads_));
    memset(segmentOffsets_, 0, sizeof(segmentOffsets_));
    memset(segmentHasPending_, 0, sizeof(segmentHasPending_));
    memset(segmentAppendable_, 0, sizeof(segmentAppendable_));
    for (auto& bytes : segmentRecordBytes_) bytes = sizeof(TelemetryRecord);
    pendingRecordCount_ = 0;
    uint64_t maximumSequence = acknowledgedSequence_;
    latestSegment_ = 0;
    bool anySegment = false;
    for (uint32_t index = 0; index < SEGMENT_LIMIT; ++index) {
        char path[20];
        segmentPath(index, path, sizeof(path));
        if (!LittleFS.exists(path)) continue;
        File file = LittleFS.open(path, FILE_READ);
        if (!file) continue;
        anySegment = true;
        latestSegment_ = max(latestSegment_, index);
        size_t offset = 0;
        TelemetryRecord record;
        while (readRecord(file, record)) {
            if (!valid(record)) break;  // Ignore a torn/corrupt tail; never overwrite it.
            segmentRecordBytes_[index] = file.position() - offset;
            maximumSequence = max(maximumSequence, record.sequence);
            if (record.sequence > acknowledgedSequence_) {
                ++pendingRecordCount_;
                if (!segmentHasPending_[index]) {
                    segmentHeads_[index] = record;
                    segmentOffsets_[index] = offset;
                    segmentHasPending_[index] = true;
                }
            }
            offset = file.position();
        }
        segmentAppendable_[index] = segmentRecordBytes_[index] == sizeof(TelemetryRecord) &&
                                    offset == file.size() &&
                                    file.size() % sizeof(TelemetryRecord) == 0 &&
                                    file.size() <= SEGMENT_BYTES;
        file.close();
    }
    if (!anySegment) latestSegment_ = 0;
    nextSequence_ = maximumSequence + 1;
    return true;
}

bool TelemetrySpool::append(const TelemetryRecord& sample) {
    if (!mounted_ || full_ || capacityTestFull_) return false;
    const uint32_t totalBytes = LittleFS.totalBytes();
    const uint32_t usedBytes = LittleFS.usedBytes();
    if (totalBytes <= usedBytes || totalBytes - usedBytes <= LITTLEFS_WRITE_RESERVE_BYTES) {
        full_ = true;
        latchStorageFullSnapshot();
        Serial.printf("Telemetry spool full: preserving %lu free LittleFS bytes.\n",
                      totalBytes > usedBytes ? static_cast<unsigned long>(totalBytes - usedBytes) : 0UL);
        return false;
    }
    if (nextSequence_ >= reservedSequenceEnd_ && !reserveSequenceBlock()) {
        full_ = true;
        return false;
    }
    TelemetryRecord record = sample;
    record.magic = RECORD_MAGIC;
    record.sequence = nextSequence_;
    record.checksum = checksum(record);

    char path[20];
    uint32_t targetSegment = latestSegment_;
    segmentPath(targetSegment, path, sizeof(path));
    File file;
    bool usableSegment = false;
    if (segmentAppendable_[targetSegment]) {
        file = LittleFS.open(path, FILE_APPEND);
        usableSegment = static_cast<bool>(file);
        if (file && (file.size() % sizeof(TelemetryRecord) != 0 ||
                     file.size() + sizeof(record) > SEGMENT_BYTES)) {
            usableSegment = false;
        }
    }
    if (!usableSegment) {
        file.close();
        file = File();
    }
    if (!file) {
        bool foundSlot = false;
        for (uint32_t index = 0; index < SEGMENT_LIMIT; ++index) {
            segmentPath(index, path, sizeof(path));
            if (LittleFS.exists(path)) continue;
            targetSegment = index;
            latestSegment_ = max(latestSegment_, index);
            file = LittleFS.open(path, FILE_WRITE);
            foundSlot = static_cast<bool>(file);
            if (!foundSlot) {
                full_ = true;
                latchStorageFullSnapshot();
                Serial.printf("Telemetry spool full: cannot create segment %lu.\n",
                              static_cast<unsigned long>(index));
                return false;
            }
            break;
        }
        if (!foundSlot) {
            full_ = true;
            latchStorageFullSnapshot();
            return false;
        }
    }
    const size_t recordOffset = file ? file.size() : 0;
    segmentRecordBytes_[targetSegment] = sizeof(TelemetryRecord);
    const bool written = file && file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
    if (file) file.close();
    if (!written) {
        full_ = true;
        latchStorageFullSnapshot();
        Serial.println("Telemetry spool full or write failed; retaining pending records and stopping new writes.");
        return false;
    }
    if (!segmentHasPending_[targetSegment]) {
        segmentHeads_[targetSegment] = record;
        segmentOffsets_[targetSegment] = recordOffset;
        segmentHasPending_[targetSegment] = true;
    }
    segmentAppendable_[targetSegment] = true;
    ++nextSequence_;
    ++pendingRecordCount_;
    if (capacityTestMode_) ++capacityTestCurrentRecordCount_;
    return true;
}

size_t TelemetrySpool::appendBatch(const TelemetryRecord* samples, size_t count) {
    if (!mounted_ || full_ || capacityTestFull_ || samples == nullptr || count == 0) return 0;
    count = min(count, static_cast<size_t>(config::TELEMETRY_FLASH_BATCH_RECORDS));

    const uint32_t totalBytes = LittleFS.totalBytes();
    const uint32_t usedBytes = LittleFS.usedBytes();
    if (totalBytes <= usedBytes || totalBytes - usedBytes <= LITTLEFS_WRITE_RESERVE_BYTES) {
        full_ = true;
        latchStorageFullSnapshot();
        Serial.printf("Telemetry spool full: preserving %lu free LittleFS bytes.\n",
                      totalBytes > usedBytes ? static_cast<unsigned long>(totalBytes - usedBytes) : 0UL);
        return 0;
    }

    if (nextSequence_ + count > reservedSequenceEnd_ && !reserveSequenceBlock()) {
        full_ = true;
        return 0;
    }

    const size_t requestedBytes = count * sizeof(TelemetryRecord);
    char path[20];
    uint32_t targetSegment = latestSegment_;
    File file;
    bool usableSegment = false;
    if (segmentAppendable_[targetSegment]) {
        segmentPath(targetSegment, path, sizeof(path));
        file = LittleFS.open(path, FILE_APPEND);
        usableSegment = static_cast<bool>(file);
        if (file && (file.size() % sizeof(TelemetryRecord) != 0 ||
                     file.size() + requestedBytes > SEGMENT_BYTES)) {
            usableSegment = false;
        }
    }
    if (!usableSegment) {
        file.close();
        file = File();
        bool foundSlot = false;
        for (uint32_t index = 0; index < SEGMENT_LIMIT; ++index) {
            segmentPath(index, path, sizeof(path));
            if (LittleFS.exists(path)) continue;
            targetSegment = index;
            latestSegment_ = max(latestSegment_, index);
            file = LittleFS.open(path, FILE_WRITE);
            foundSlot = static_cast<bool>(file);
            if (!foundSlot) {
                full_ = true;
                latchStorageFullSnapshot();
                Serial.printf("Telemetry spool full: cannot create segment %lu.\n",
                              static_cast<unsigned long>(index));
                return 0;
            }
            break;
        }
        if (!foundSlot) {
            full_ = true;
            latchStorageFullSnapshot();
            return 0;
        }
    }

    segmentRecordBytes_[targetSegment] = sizeof(TelemetryRecord);
    const size_t recordOffset = file.size();
    TelemetryRecord batch[config::TELEMETRY_FLASH_BATCH_RECORDS] = {};
    for (size_t i = 0; i < count; ++i) {
        batch[i] = samples[i];
        batch[i].magic = RECORD_MAGIC;
        batch[i].sequence = nextSequence_ + i;
        batch[i].checksum = checksum(batch[i]);
    }
    const size_t bytesWritten = file.write(reinterpret_cast<const uint8_t*>(batch), requestedBytes);
    if (file) file.close();

    const size_t committed = min(count, bytesWritten / sizeof(TelemetryRecord));
    if (committed > 0) {
        if (!segmentHasPending_[targetSegment]) {
            segmentHeads_[targetSegment] = batch[0];
            segmentOffsets_[targetSegment] = recordOffset;
            segmentHasPending_[targetSegment] = true;
        }
        nextSequence_ += committed;
        pendingRecordCount_ += committed;
        if (capacityTestMode_) capacityTestCurrentRecordCount_ += committed;
    }

    segmentAppendable_[targetSegment] = bytesWritten == requestedBytes;
    if (committed != count) {
        Serial.printf("Telemetry spool batch short write: %lu/%lu records committed.\n",
                      static_cast<unsigned long>(committed), static_cast<unsigned long>(count));
        if (committed == 0) {
            full_ = true;
            latchStorageFullSnapshot();
        }
    }
    return committed;
}

bool TelemetrySpool::findPending(TelemetryRecord& sample, uint32_t* foundSegment, size_t* foundOffset) {
    bool found = false;
    uint32_t oldestSegment = 0;
    for (uint32_t index = 0; index < SEGMENT_LIMIT; ++index) {
        if (!segmentHasPending_[index]) continue;
        if (!found || segmentHeads_[index].sequence < segmentHeads_[oldestSegment].sequence) {
            oldestSegment = index;
            found = true;
        }
    }
    if (found) {
        sample = segmentHeads_[oldestSegment];
        if (foundSegment) *foundSegment = oldestSegment;
        if (foundOffset) *foundOffset = segmentOffsets_[oldestSegment];
    }
    return found;
}

void TelemetrySpool::advanceSegment(uint32_t segment) {
    char path[20];
    segmentPath(segment, path, sizeof(path));
    File file = LittleFS.open(path, FILE_READ);
    if (!file) {
        segmentHasPending_[segment] = false;
        return;
    }
    size_t offset = segmentOffsets_[segment] + segmentRecordBytes_[segment];
    if (!file.seek(offset, SeekSet)) {
        file.close();
        segmentHasPending_[segment] = false;
        return;
    }
    TelemetryRecord record;
    while (readRecord(file, record)) {
        if (!valid(record)) break;
        if (record.sequence > acknowledgedSequence_) {
            segmentHeads_[segment] = record;
            segmentOffsets_[segment] = offset;
            segmentHasPending_[segment] = true;
            file.close();
            return;
        }
        offset = file.position();
    }
    file.close();
    segmentHasPending_[segment] = false;
}

bool TelemetrySpool::peek(TelemetryRecord& sample) {
    return mounted_ && findPending(sample, nullptr, nullptr);
}

size_t TelemetrySpool::peekBatch(TelemetryRecord* records, size_t capacity) {
    if (!mounted_ || !records || capacity == 0) return 0;

    uint32_t segment = 0;
    size_t offset = 0;
    TelemetryRecord first;
    if (!findPending(first, &segment, &offset)) return 0;

    char path[20];
    segmentPath(segment, path, sizeof(path));
    File file = LittleFS.open(path, FILE_READ);
    if (!file || !file.seek(offset, SeekSet)) {
        if (file) file.close();
        return 0;
    }

    size_t count = 0;
    TelemetryRecord record;
    while (count < capacity && readRecord(file, record)) {
        if (!valid(record) || record.sequence <= acknowledgedSequence_) break;
        records[count++] = record;
    }
    file.close();
    return count;
}

uint64_t TelemetrySpool::pendingCount() const {
    return mounted_ ? pendingRecordCount_ : 0;
}

void TelemetrySpool::latchCapacityTestFull() {
    if (!capacityTestMode_ || capacityTestResultSaved_) return;

    if (!capacityTestFull_) {
        capacityTestFull_ = true;
        full_ = true;
        capacityTestRecordCount_ = pendingCount();
        capacityTestUsedBytes_ = static_cast<uint32_t>(LittleFS.usedBytes());
        capacityTestTotalBytes_ = static_cast<uint32_t>(LittleFS.totalBytes());
    }

    const bool saved = preferences.putULong64("ctcount", capacityTestRecordCount_) == sizeof(uint64_t) &&
                       preferences.putUInt("ctused", capacityTestUsedBytes_) > 0 &&
                       preferences.putUInt("cttotal", capacityTestTotalBytes_) > 0 &&
                       preferences.putBool("ctfull", true) > 0;
    capacityTestResultSaved_ = saved;
    Serial.printf("CAPACITY_TEST_FULL records=%llu used=%lu/%lu bytes saved=%s\n",
                  static_cast<unsigned long long>(capacityTestRecordCount_),
                  static_cast<unsigned long>(capacityTestUsedBytes_),
                  static_cast<unsigned long>(capacityTestTotalBytes_), saved ? "yes" : "no");
}

bool TelemetrySpool::acknowledge(uint64_t sequence) {
    if (sequence <= acknowledgedSequence_) return true;
    TelemetryRecord pending;
    uint32_t segment = 0;
    if (!findPending(pending, &segment, nullptr) || pending.sequence != sequence) return false;
    const uint64_t previousAcknowledgedSequence = acknowledgedSequence_;
    acknowledgedSequence_ = sequence;
    advanceSegment(segment);
    if (!segmentHasPending_[segment]) {
        if (persistedAcknowledgedSequence_ < acknowledgedSequence_) {
            if (preferences.putULong64("acked", acknowledgedSequence_) != sizeof(uint64_t)) {
                // Restore the pending segment in RAM so a failed NVS write can be retried.
                acknowledgedSequence_ = previousAcknowledgedSequence;
                scan();
                return false;
            }
            persistedAcknowledgedSequence_ = acknowledgedSequence_;
        }
        char path[20];
        segmentPath(segment, path, sizeof(path));
        if (LittleFS.remove(path)) {
            segmentAppendable_[segment] = false;
            if (!capacityTestFull_) full_ = false;
            clearStorageFullSnapshotIfSpaceAvailable();
        }
    }
    if (pendingRecordCount_ > 0) --pendingRecordCount_;
    return true;
}

bool TelemetrySpool::compactAcknowledgedSegments() {
    bool changed = false;
    for (uint32_t index = 0; index <= latestSegment_; ++index) {
        if (segmentHasPending_[index]) continue;
        char path[20];
        segmentPath(index, path, sizeof(path));
        File file = LittleFS.open(path, FILE_READ);
        if (!file) continue;
        bool hasValidRecord = false;
        bool allAcknowledged = true;
        TelemetryRecord record;
        while (readRecord(file, record)) {
            if (!valid(record)) break;
            hasValidRecord = true;
            if (record.sequence > acknowledgedSequence_) allAcknowledged = false;
        }
        file.close();
        if (hasValidRecord && allAcknowledged) {
            // Persist the reclaim watermark before deleting the segment. If power fails,
            // the PC's receipt table makes any replay safe and idempotent.
            if (persistedAcknowledgedSequence_ < acknowledgedSequence_) {
                if (preferences.putULong64("acked", acknowledgedSequence_) != sizeof(uint64_t)) {
                    return changed;
                }
                persistedAcknowledgedSequence_ = acknowledgedSequence_;
            }
            if (LittleFS.remove(path)) {
                segmentAppendable_[index] = false;
                changed = true;
            }
        }
    }
    if (changed) full_ = false;
    if (changed) clearStorageFullSnapshotIfSpaceAvailable();
    return changed;
}
