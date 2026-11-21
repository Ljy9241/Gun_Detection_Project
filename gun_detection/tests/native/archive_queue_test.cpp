// Runs the production queue and Flash spool against an in-memory filesystem/NVS.
#include "archive_queue.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <cassert>
#include <iostream>

static void resetStorage() {
    storage_test::files.clear();
    storage_test::nvs.clear();
    storage_test::failKey.clear();
    storage_test::failRead = storage_test::failRemove = false;
    storage_test::shortWrite = SIZE_MAX;
    storage_test::missingReadOpens = 0;
    storage_test::failReadPath.clear();
    storage_test::readFailureAfterAck.clear();
    storage_test::ackWrites = 0;
    storage_test::failStatPath.clear();
    storage_test::statCalls = storage_test::readOpens = storage_test::existsCalls = 0;
}

static TelemetryRecord row(uint8_t kind = 1, uint32_t shot = 0) {
    TelemetryRecord r;
    r.bootId = 123;
    r.recordKind = kind;
    r.shotId = shot;
    return r;
}

static void drain(TelemetrySpool& spool, ArchiveQueue& queue) {
    unsigned batches = 0;
    while (queue.prepare(spool)) {
        assert(++batches < 1000);
        assert(queue.confirm(spool));
    }
    assert(spool.pendingCount() == 0);
    assert(storage_test::files.empty());
}

static void fragmentRecords(unsigned count) {
    const auto original = *storage_test::files.at("/q00000000.bin");
    storage_test::files.clear();
    for (unsigned i = 0; i < count; ++i) {
        char path[20];
        snprintf(path, sizeof path, "/q%08u.bin", (i * 53) % count);
        storage_test::files[path] = std::make_shared<std::vector<uint8_t>>(
            original.begin() + i * sizeof(TelemetryRecord),
            original.begin() + (i + 1) * sizeof(TelemetryRecord));
    }
}

int main() {
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        const unsigned mountStats = storage_test::statCalls;
        for (int i = 0; i < 3; ++i) assert(!spool.reclaimAcknowledged());
        assert(storage_test::statCalls == mountStats && storage_test::readOpens == 0);
        assert(storage_test::existsCalls == 0 && storage_test::missingReadOpens == 0);
        for (int i = 0; i < 728; ++i) assert(spool.append(row()));
        const unsigned opens = storage_test::readOpens;
        assert(!spool.reclaimAcknowledged()); // All known files still contain unconfirmed records.
        TelemetryRecord shots[64];
        size_t count;
        uint64_t scanned;
        assert(spool.readShots(0, spool.lastSequence(), shots, 64, count, scanned));
        assert(count == 0 && storage_test::readOpens == opens); // No scanning sensor-only files.
        assert(spool.append(row(0, 1)));
        assert(spool.readShots(0, spool.lastSequence(), shots, 64, count, scanned));
        assert(count == 1 && shots[0].shotId == 1 && storage_test::readOpens == opens + 1);
        TelemetryRecord extra[] = {row(), row(0, 2)};
        assert(spool.appendBatch(extra, 2) == 2);
        TelemetrySpool reboot;
        assert(reboot.begin());
        const unsigned reopened = storage_test::readOpens;
        assert(reboot.readShots(0, reboot.lastSequence(), shots, 64, count, scanned));
        assert(count == 2 && storage_test::readOpens == reopened + 1);
        ArchiveQueue queue;
        drain(reboot, queue);
        const unsigned reclaimedOpens = storage_test::readOpens, reclaimedStats = storage_test::statCalls;
        assert(!reboot.reclaimAcknowledged());
        assert(storage_test::readOpens == reclaimedOpens && storage_test::statCalls == reclaimedStats);
        assert(storage_test::existsCalls == 0 && storage_test::missingReadOpens == 0);
    }
    resetStorage();
    {
        TelemetrySpool original;
        assert(original.begin() && original.append(row()));
        const auto bytes = *storage_test::files.at("/q00000000.bin");
        storage_test::failStatPath = "/littlefs/q00000000.bin";
        TelemetrySpool failed;
        assert(!failed.begin());
        assert(!failed.append(row())); // Never mistake an unreadable slot for a free slot.
        assert(*storage_test::files.at("/q00000000.bin") == bytes);
        storage_test::failStatPath.clear();
        TelemetrySpool recovered;
        assert(recovered.begin() && recovered.pendingCount() == 1);
    }
    resetStorage();
    {
        TelemetrySpool original;
        assert(original.begin());
        for (int i = 0; i < 128; ++i) assert(original.append(row()));
        fragmentRecords(128); // Existing one-record files, deliberately out of index order.
        TelemetrySpool spool;
        assert(spool.begin());
        ArchiveQueue queue;
        unsigned requests = 0;
        while (queue.prepare(spool)) {
            assert(!queue.priority && queue.count == 64);
            for (size_t i = 0; i < queue.count; ++i) assert(queue.rows[i].sequence == requests * 64 + i + 1);
            assert(queue.confirm(spool));
            ++requests;
        }
        assert(requests == 2 && storage_test::files.empty());
        assert(storage_test::ackWrites == 2); // One durable write per verified batch.
        std::cout << "128 fragmented records: 128 single-file requests -> " << requests << " cross-file requests\n";
    }
    resetStorage();
    {
        TelemetrySpool original;
        assert(original.begin());
        for (int i = 0; i < 128; ++i) assert(original.append(row()));
        fragmentRecords(128);
        TelemetrySpool spool;
        assert(spool.begin());
        ArchiveQueue queue;
        assert(queue.prepare(spool) && queue.count == 64);
        // Validate the entire receipt before deleting any segment.
        ++queue.rows[63].bootId;
        assert(!queue.confirm(spool));
        assert(spool.pendingCount() == 128 && storage_test::files.size() == 128);
        --queue.rows[63].bootId;
        storage_test::failKey = "acked";
        assert(!queue.confirm(spool) && spool.pendingCount() == 128);
        storage_test::failKey.clear();
        storage_test::readFailureAfterAck = "/q00000053.bin"; // Second record's file.
        assert(!queue.confirm(spool));
        assert(spool.pendingCount() == 127 && storage_test::files.size() == 127);
        assert(storage_test::nvs.at("acked") == 64);
        storage_test::failReadPath.clear();
        storage_test::readFailureAfterAck.clear();
        assert(queue.prepare(spool) && queue.count == 64 && queue.rows[0].sequence == 1);
        assert(spool.append(row())); // A new record during receipt retry must survive.
        assert(queue.confirm(spool) && spool.pendingCount() == 65);
        drain(spool, queue);
    }
    resetStorage();
    {
        TelemetrySpool original;
        assert(original.begin());
        for (int i = 0; i < 4; ++i) assert(original.append(row()));
        const auto bytes = *storage_test::files.at("/q00000000.bin");
        auto& a = *storage_test::files.at("/q00000000.bin");
        a.assign(bytes.begin(), bytes.begin() + sizeof(TelemetryRecord));
        a.insert(a.end(), bytes.begin() + 3 * sizeof(TelemetryRecord), bytes.end());
        storage_test::files["/q00000001.bin"] = std::make_shared<std::vector<uint8_t>>(
            bytes.begin() + sizeof(TelemetryRecord), bytes.begin() + 3 * sizeof(TelemetryRecord));
        TelemetrySpool spool;
        assert(spool.begin());
        ArchiveQueue queue;
        assert(queue.prepare(spool) && queue.count == 4);
        for (size_t i = 0; i < 4; ++i) assert(queue.rows[i].sequence == i + 1);
        assert(queue.confirm(spool) && spool.pendingCount() == 0 && storage_test::files.empty());
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        // Two full segments, then recycle index 0 while index 1 still holds data.
        for (int i = 0; i < 728; ++i) assert(spool.append(row()));
        while (storage_test::files.count("/q00000000.bin")) {
            TelemetryRecord records[20];
            uint64_t sequences[20];
            const size_t count = spool.peekBatch(records, 20);
            assert(count > 0);
            for (size_t i = 0; i < count; ++i) sequences[i] = records[i].sequence;
            assert(spool.acknowledgeBatch(sequences, count));
        }
        assert(spool.append(row()));
        assert(spool.append(row()));
        TelemetryRecord pair[] = {row(), row()};
        assert(spool.appendBatch(pair, 2) == 2);
        assert(storage_test::files.size() == 2);
        assert(storage_test::files.at("/q00000000.bin")->size() == 4 * sizeof(TelemetryRecord));
        // Restart must recover the newest segment by sequence, not filename.
        TelemetrySpool reboot;
        assert(reboot.begin() && reboot.append(row()));
        assert(storage_test::files.size() == 2);
        assert(storage_test::files.at("/q00000000.bin")->size() == 5 * sizeof(TelemetryRecord));
        ArchiveQueue queue;
        drain(reboot, queue);
        assert(!reboot.reclaimAcknowledged());
        assert(storage_test::missingReadOpens == 0);
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        // Regression: unsealed shot + invalid GPS + legacy row must all upload.
        assert(spool.append(row()));
        assert(spool.append(row(2)));
        assert(spool.append(row(0, 1)));
        assert(spool.append(row(0)));
        assert(spool.sealedThrough() == 0);
        // Watermarks from the old firmware must not gate the new complete queue.
        assert(spool.confirmOrdinary(spool.lastSequence()));
        assert(spool.confirmGps(spool.lastSequence()));
        ArchiveQueue queue;
        assert(queue.prepare(spool) && queue.priority && queue.count == 1);
        assert(queue.rows[0].shotId == 1);
        assert(queue.confirm(spool));
        assert(spool.pendingCount() == 4); // Priority ACK cannot skip unsent rows.
        assert(queue.prepare(spool) && !queue.priority && queue.count == 4);
        auto snapshot = queue.rows[3].sequence;
        assert(spool.append(row(0, 2))); // Arrives while HTTPS is in flight.
        assert(queue.prepare(spool) && queue.count == 4 && queue.rows[3].sequence == snapshot);
        assert(queue.confirm(spool));
        assert(spool.pendingCount() == 1 && !storage_test::files.empty());
        drain(spool, queue);
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        for (int i = 0; i < 400; ++i) assert(spool.append(row()));
        assert(spool.append(row(0, 1)));
        ArchiveQueue queue;
        assert(queue.prepare(spool) && queue.priority && queue.rows[0].shotId == 1);
        assert(queue.confirm(spool));
        assert(queue.prepare(spool) && !queue.priority); // FIFO cannot starve.
        assert(queue.confirm(spool));
        assert(spool.append(row(0, 2)));
        assert(queue.prepare(spool) && queue.priority && queue.rows[0].shotId == 2);
        assert(queue.confirm(spool));
        drain(spool, queue); // Reclaims more than one 32 KiB segment.
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        assert(spool.append(row()));
        ArchiveQueue queue;
        assert(queue.prepare(spool));
        auto seq = queue.rows[0].sequence;
        const uint64_t wrong = seq + 1;
        assert(!spool.acknowledgeBatch(&wrong, 1));
        assert(spool.pendingCount() == 1);
        // Lost receipt / network timeout: no confirm, same snapshot retried.
        assert(queue.prepare(spool) && queue.rows[0].sequence == seq);
        storage_test::failKey = "acked";
        assert(!queue.confirm(spool));
        assert(spool.pendingCount() == 1 && !storage_test::files.empty());
        storage_test::failKey.clear();
        assert(queue.confirm(spool) && storage_test::files.empty());
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        assert(spool.append(row()));
        assert(spool.append(row()));
        TelemetryRecord first;
        assert(spool.peek(first));
        storage_test::failRead = true;
        assert(!spool.acknowledge(first.sequence)); // Cannot treat I/O failure as EOF.
        assert(!storage_test::files.empty() && spool.pendingCount() == 2);
        storage_test::failRead = false;
        ArchiveQueue queue;
        drain(spool, queue); // Transient I/O failure can retry without a reboot.
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        constexpr size_t total = ArchiveQueue::capacity + 5;
        for (size_t i = 0; i < total; ++i) assert(spool.append(row()));
        ArchiveQueue queue;
        assert(queue.prepare(spool) && queue.count == ArchiveQueue::capacity);
        assert(queue.confirm(spool));
        assert(spool.pendingCount() == 5 && !storage_test::files.empty());
        // Simulate power loss before a segment is fully acknowledged.
        TelemetrySpool reboot;
        assert(reboot.begin() && reboot.pendingCount() == 5); // Durable batch cursor survives reboot.
        assert(reboot.append(row(0, 7)));
        ArchiveQueue restarted;
        drain(reboot, restarted);
        TelemetrySpool afterDelete;
        assert(afterDelete.begin() && afterDelete.pendingCount() == 0);
        assert(afterDelete.append(row()));
        TelemetryRecord next;
        assert(afterDelete.peek(next) && next.sequence > total);
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin() && spool.append(row()));
        ArchiveQueue queue;
        assert(queue.prepare(spool));
        storage_test::failRemove = true;
        assert(queue.confirm(spool));
        assert(spool.pendingCount() == 0 && !storage_test::files.empty());
        assert(!spool.reclaimAcknowledged());
        storage_test::failRemove = false;
        assert(spool.reclaimAcknowledged() && storage_test::files.empty());
    }
    resetStorage();
    {
        TelemetrySpool spool;
        assert(spool.begin());
        assert(spool.append(row()));
        storage_test::shortWrite = 10;
        assert(!spool.append(row()));
        storage_test::shortWrite = SIZE_MAX;
        ArchiveQueue queue;
        assert(queue.prepare(spool));
        assert(!queue.confirm(spool)); // Corrupt tail is retained, never silently erased.
        assert(!storage_test::files.empty());
    }
    std::cout << "archive_queue_test: all cases passed\n";
}
