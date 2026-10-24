#include "telemetry_spool.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

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

void TelemetrySpool::segmentPath(uint32_t index, char* path, size_t size) {
    snprintf(path, size, "/q%08lu.bin", static_cast<unsigned long>(index));
}

bool TelemetrySpool::begin() {
    if (!preferences.begin("tlm-spool", false)) {
        Serial.println("Telemetry spool: NVS open failed.");
        return false;
    }
    const bool firstInitialization = !preferences.getBool("fsinit", false);
    mounted_ = LittleFS.begin(firstInitialization);
    if (!mounted_) {
        Serial.println("Telemetry spool: LittleFS mount failed; flash was not formatted.");
        return false;
    }
    if (firstInitialization && preferences.putBool("fsinit", true) == 0) {
        Serial.println("Telemetry spool: could not persist filesystem initialization marker.");
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
    uint64_t maximumSequence = acknowledgedSequence_;
    latestSegment_ = 0;
    bool anySegment = false;
    for (uint32_t index = 0; index < SEGMENT_LIMIT; ++index) {
        char path[20];
        segmentPath(index, path, sizeof(path));
        File file = LittleFS.open(path, FILE_READ);
        if (!file) continue;
        anySegment = true;
        latestSegment_ = max(latestSegment_, index);
        size_t offset = 0;
        TelemetryRecord record;
        while (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
            if (!valid(record)) break;  // Ignore a torn/corrupt tail; never overwrite it.
            maximumSequence = max(maximumSequence, record.sequence);
            if (!segmentHasPending_[index] && record.sequence > acknowledgedSequence_) {
                segmentHeads_[index] = record;
                segmentOffsets_[index] = offset;
                segmentHasPending_[index] = true;
            }
            offset += sizeof(record);
        }
        file.close();
    }
    if (!anySegment) latestSegment_ = 0;
    nextSequence_ = maximumSequence + 1;
    return true;
}

bool TelemetrySpool::append(const TelemetryRecord& sample) {
    if (!mounted_ || full_) return false;
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
    File file = LittleFS.open(path, FILE_APPEND);
    bool usableSegment = static_cast<bool>(file);
    if (file && (file.size() % sizeof(TelemetryRecord) != 0 ||
                 file.size() + sizeof(record) > SEGMENT_BYTES)) {
        usableSegment = false;
    }
    if (usableSegment) {
        File check = LittleFS.open(path, FILE_READ);
        if (!check) usableSegment = false;
        TelemetryRecord existing;
        while (check && check.read(reinterpret_cast<uint8_t*>(&existing), sizeof(existing)) == sizeof(existing)) {
            if (!valid(existing)) {
                usableSegment = false;
                break;
            }
        }
        if (check) check.close();
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
            if (foundSlot) break;
        }
        if (!foundSlot) {
            full_ = true;
            return false;
        }
    }
    const size_t recordOffset = file ? file.size() : 0;
    const bool written = file && file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
    if (file) file.close();
    if (!written) {
        full_ = true;
        Serial.println("Telemetry spool full or write failed; retaining pending records and stopping new writes.");
        return false;
    }
    if (!segmentHasPending_[targetSegment]) {
        segmentHeads_[targetSegment] = record;
        segmentOffsets_[targetSegment] = recordOffset;
        segmentHasPending_[targetSegment] = true;
    }
    ++nextSequence_;
    return true;
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
    size_t offset = segmentOffsets_[segment] + sizeof(TelemetryRecord);
    if (!file.seek(offset, SeekSet)) {
        file.close();
        segmentHasPending_[segment] = false;
        return;
    }
    TelemetryRecord record;
    while (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
        if (!valid(record)) break;
        if (record.sequence > acknowledgedSequence_) {
            segmentHeads_[segment] = record;
            segmentOffsets_[segment] = offset;
            segmentHasPending_[segment] = true;
            file.close();
            return;
        }
        offset += sizeof(record);
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
    while (count < capacity && file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
        if (!valid(record) || record.sequence <= acknowledgedSequence_) break;
        records[count++] = record;
    }
    file.close();
    return count;
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
        if (LittleFS.remove(path)) full_ = false;
    }
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
        while (file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
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
            if (LittleFS.remove(path)) changed = true;
        }
    }
    if (changed) full_ = false;
    return changed;
}
