#pragma once

#include "system_config.h"
#include "telemetry_spool.h"

// A snapshot remains unchanged until its exact cloud receipt has been verified.
// Priority delivery never acknowledges an intervening, unsent Flash record.
class ArchiveQueue {
public:
    static constexpr size_t capacity = config::ARCHIVE_UPLOAD_BATCH_RECORDS;
    TelemetryRecord rows[capacity] = {};
    size_t count = 0;
    bool priority = false;

    bool prepare(TelemetrySpool& spool) {
        if (count) return true;  // Retry the identical snapshot after any failure.
        priority = false;
        if (preferShots_ && spool.lastSequence() > shotScan_) {
            if (!spool.readShots(shotScan_, spool.lastSequence(), rows, capacity, count, scanned_)) {
                count = 0;
                return false;
            }
            if (count) {
                priority = true;
                return true;
            }
            shotScan_ = scanned_;  // No shot identities were skipped.
        }
        // Every record kind is eligible while collection is still running.
        count = spool.peekArchiveBatch(rows, capacity);
        return count != 0;
    }

    // Call ONLY after validating committed=true and every returned identity.
    bool confirm(TelemetrySpool& spool) {
        if (!count) return false;
        if (priority) {
            shotScan_ = scanned_;
        } else {
            if (!spool.acknowledgeArchiveBatch(rows, count)) return false;
        }
        // Alternate priority and FIFO batches so a backlog always drains.
        // FIFO may replay a priority shot; the server deduplicates its identity.
        preferShots_ = !priority;
        count = 0;
        return true;
    }

private:
    uint64_t shotScan_ = 0, scanned_ = 0;
    bool preferShots_ = true;
};
