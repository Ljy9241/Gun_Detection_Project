#ifdef DEVICE_ARCHIVE_UPLOAD
#include "archive_upload.h"
#include "archive_queue.h"
#include "archive_time.h"
#include "cloud_device_private.h"
#include <esp_http_client.h>
#include "system_config.h"
#include <cJSON.h>
#include <memory>
#include <time.h>

namespace {
ArchiveQueue batch;
struct UploadTime {
    char timestamp[20] = {};
    TelemetryTimeAnchor anchor;
    bool inferred = false;
};
UploadTime uploadTimes[ArchiveQueue::capacity];
bool timesPrepared = false;

void prepareTimes() {
    if (timesPrepared) return;
    for (size_t i = 0; i < batch.count; ++i) {
        const auto& r = batch.rows[i];
        auto& t = uploadTimes[i];
        t = {};
        memcpy(t.timestamp, r.timestamp, sizeof t.timestamp);
        if (!strncmp(r.timestamp, "T+", 2) && readArchiveTime(r.bootId, t.anchor))
            t.inferred = inferTelemetryTime(t.anchor, r.bootId, r.uptimeMs,
                                           t.timestamp, sizeof t.timestamp);
    }
    timesPrepared = true;
}
QueueHandle_t results = nullptr;
QueueHandle_t requests = nullptr;
bool workerStarted = false;
bool busy = false;
bool testReplay = false;
uint32_t nextAttempt = 0;

struct UploadResult {
    bool ok = false;
    uint32_t elapsedMs = 0, payloadBytes = 0, connections = 0;
    uint32_t inserted = 0, duplicates = 0;
};

// Only the persistent network worker accesses the client and these buffers.
esp_http_client_handle_t http = nullptr;
char response[6144];
size_t responseSize = 0;
bool responseOverflow = false;
uint32_t connections = 0;

esp_err_t httpEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) ++connections;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        const size_t length = static_cast<size_t>(event->data_len);
        if (length >= sizeof(response) - responseSize) {
            responseOverflow = true;
            return ESP_FAIL;
        }
        memcpy(response + responseSize, event->data, length);
        responseSize += length;
    }
    return ESP_OK;
}

void closeClient() {
    if (http) esp_http_client_cleanup(http);
    http = nullptr;
}

bool matches(const cJSON* obj, const char* key, const char* expected) {
    const char* value = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
    return value && strcmp(value, expected) == 0;
}

bool committed(const char* json, const char* id, UploadResult& result) {
    std::unique_ptr<cJSON, decltype(&cJSON_Delete)> root(cJSON_Parse(json), cJSON_Delete);
    const cJSON* ack = root.get();
    const cJSON* records = cJSON_GetObjectItemCaseSensitive(ack, "records");
    const cJSON* count = cJSON_GetObjectItemCaseSensitive(ack, "acceptedCount");
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ack, "committed")) ||
        !matches(ack, "deviceId", archiveDevice) || !matches(ack, "storageEpoch", archiveEpoch) ||
        !matches(ack, "batchId", id) || !cJSON_IsNumber(count) || count->valuedouble != batch.count ||
        !cJSON_IsArray(records) || cJSON_GetArraySize(records) != static_cast<int>(batch.count)) return false;
    for (size_t i = 0; i < batch.count; ++i) {
        char boot[17], seq[21];
        snprintf(boot, sizeof boot, "%016llx", batch.rows[i].bootId);
        snprintf(seq, sizeof seq, "%llu", batch.rows[i].sequence);
        const cJSON* record = cJSON_GetArrayItem(records, i);
        if (!matches(record, "bootId", boot) || !matches(record, "sequence", seq)) return false;
    }
    const cJSON* inserted = cJSON_GetObjectItemCaseSensitive(ack, "insertedCount");
    const cJSON* duplicates = cJSON_GetObjectItemCaseSensitive(ack, "duplicateCount");
    if (!cJSON_IsNumber(inserted) || !cJSON_IsNumber(duplicates) ||
        inserted->valuedouble < 0 || duplicates->valuedouble < 0 ||
        inserted->valuedouble + duplicates->valuedouble != batch.count) return false;
    result.inserted = inserted->valueint;
    result.duplicates = duplicates->valueint;
    return true;
}

bool postBatch(UploadResult& result) {
    char id[64];
    snprintf(id, sizeof id, "%c-%016llx-%llu-%llu", batch.priority ? 's' : 'q', batch.rows[0].bootId, batch.rows[0].sequence,
             batch.rows[batch.count - 1].sequence);
    static char body[32768];
    int pos = snprintf(body, sizeof body,
        "{\"schemaVersion\":1,\"deviceId\":\"%s\",\"storageEpoch\":\"%s\",\"batchId\":\"%s\","
        "\"format\":\"compact-v4-time\",\"records\":[", archiveDevice, archiveEpoch, id);
    if (pos <= 0 || size_t(pos) >= sizeof body) return false;
    for (size_t i = 0; i < batch.count; ++i) {
        const auto& r = batch.rows[i];
        char lat[24] = "null", lon[24] = "null";
        if (r.gpsValid) {
            snprintf(lat, sizeof lat, "%.7f", r.latitude);
            snprintf(lon, sizeof lon, "%.7f", r.longitude);
        }
        char axes[64];
        const bool tilt = r.reserved[0] == 1;
        if (tilt && r.accelZ == 1) {
            snprintf(axes, sizeof axes, "%.2f,%.2f,true", r.accelX / 100.0 - 180.0,
                     r.accelY / 100.0 - 90.0);
        } else if (tilt) {
            strcpy(axes, "null,null,false");
        } else {
            snprintf(axes, sizeof axes, "%u,%u,%u", r.accelX, r.accelY, r.accelZ);
        }
        const auto& t = uploadTimes[i];
        char provenance[112] = "null,null,null";
        if (t.inferred) snprintf(provenance, sizeof provenance, "\"%.19s\",\"%llu\",\"%llu\"",
                                 r.timestamp, t.anchor.utcMs, t.anchor.uptimeMs);
        int size = snprintf(body + pos, sizeof body - pos,
            "%s[\"%llu\",\"%.19s\",%s,%u,%u,%.3f,%s,%s,\"%016llx\",\"%llu\",%lu,%u,%u,%s]",
            i ? "," : "", r.sequence, t.timestamp, axes, r.micRaw,
            r.micP2p, r.micRms, lat, lon, r.bootId, r.uptimeMs, (unsigned long)r.shotId,
            r.recordKind, tilt ? 1 : 0, provenance);
        if (size < 0 || size_t(size) + 3 > sizeof body - pos) return false;
        pos += size;
    }
    strcpy(body + pos, "]}");
    if (!http) {
        esp_http_client_config_t config{};
        config.url = "https://39.106.146.122/device/v1/batches";
        config.cert_pem = archiveRoot;
        config.timeout_ms = 10000;
        config.method = HTTP_METHOD_POST;
        config.disable_auto_redirect = true;
        config.keep_alive_enable = true;
        config.event_handler = httpEvent;
        http = esp_http_client_init(&config);
        const String auth = String("Bearer ") + archiveToken;
        if (!http || esp_http_client_set_header(http, "Authorization", auth.c_str()) != ESP_OK ||
            esp_http_client_set_header(http, "Content-Type", "application/json") != ESP_OK) return false;
    }
    const int length = pos + 2;
    result.payloadBytes = length;
    responseSize = 0;
    responseOverflow = false;
    connections = 0;
    if (esp_http_client_set_post_field(http, body, length) != ESP_OK) return false;
    // perform() completes the response and leaves a reusable connection open
    // when the server permits it. The callback also handles chunked responses.
    const esp_err_t sent = esp_http_client_perform(http);
    result.connections = connections;
    const int status = esp_http_client_get_status_code(http);
    if (sent != ESP_OK || status != 200 || responseOverflow || !responseSize ||
        !esp_http_client_is_complete_data_received(http)) {
        Serial.printf("Archive upload failed: error=%s status=%d; kept in Flash.\n",
                      esp_err_to_name(sent), status);
        return false;
    }
    response[responseSize] = 0;
    return committed(response, id, result);
}

void sendTask(void*) {
    bool request;
    for (;;) {
        if (!xQueueReceive(requests, &request, pdMS_TO_TICKS(15000))) {
            closeClient(); // Release TLS heap when idle/offline, without touching Flash.
            continue;
        }
        UploadResult result;
        const uint32_t started = millis();
        result.ok = postBatch(result);
        result.elapsedMs = millis() - started;
        if (!result.ok) closeClient(); // Retry the retained snapshot on a fresh connection.
        xQueueSend(results, &result, portMAX_DELAY);
    }
}

bool startBatch() {
    const bool request = true;
    return xQueueSend(requests, &request, 0) == pdTRUE;
}
}

void testArchiveReplayOnce() {
    testReplay = true;
}

void updateArchiveUpload(TelemetrySpool& spool) {
    static uint32_t nextReclaim = 0;
    static uint32_t speedStart = millis(), nextIdlePoll = 0;
    static uint64_t idleSequence = UINT64_MAX;
    static uint32_t speedFifo = 0, speedInserted = 0, speedDuplicates = 0;
    if (!results) results = xQueueCreate(1, sizeof(UploadResult));
    if (!requests) requests = xQueueCreate(1, sizeof(bool));
    if (!results || !requests) return;
    if (!workerStarted) {
        workerStarted = xTaskCreate(sendTask, "archive-https", 16384, nullptr, 1, nullptr) == pdPASS;
        if (!workerStarted) return;
    }
    bool ok = false;
    if (busy) {
        UploadResult result;
        if (!xQueueReceive(results, &result, 0)) return;
        ok = result.ok;
        busy = false;
        if (ok) {
            speedInserted += result.inserted;
            speedDuplicates += result.duplicates;
        }
        if (ok && testReplay) {
            testReplay = false;
            Serial.println("USB test: retaining confirmed batch for one repeat.");
            // Resend the identical snapshot even if newer rows have arrived.
            busy = startBatch();
            return;
        }
        if (ok) {
            const size_t count = batch.count;
            const uint64_t through = batch.rows[count - 1].sequence;
            ok = batch.confirm(spool);
            if (ok) timesPrepared = false;
            if (ok && !batch.priority) speedFifo += count;
            if (ok) Serial.printf("%s committed: %u records through %llu; flash_pending=%llu inserted=%lu duplicates=%lu http_ms=%lu new_connections=%lu\n",
                batch.priority ? "Shot priority" : "Archive", unsigned(count), through,
                spool.pendingCount(), (unsigned long)result.inserted, (unsigned long)result.duplicates,
                (unsigned long)result.elapsedMs, (unsigned long)result.connections);
        }
        nextAttempt = millis() + (ok ? 0 : 1000);
    }
    const uint32_t now = millis();
    if (now - speedStart >= 10000) {
        const float seconds = (now - speedStart) / 1000.0F;
        Serial.printf("Archive speed: fifo=%.2f records/s inserted=%.2f records/s duplicates=%lu window_ms=%lu flash_pending=%llu used=%lu/%lu\n",
                      speedFifo / seconds, speedInserted / seconds,
                      (unsigned long)speedDuplicates, (unsigned long)(now - speedStart),
                      spool.pendingCount(), (unsigned long)spool.storageUsedBytes(),
                      (unsigned long)spool.storageTotalBytes());
        speedStart = now;
        speedFifo = speedInserted = speedDuplicates = 0;
    }
    if (int32_t(millis() - nextReclaim) >= 0) {
        spool.reclaimAcknowledged();  // Retry a failed file removal without retransmitting data.
        nextReclaim = millis() + 30000;
    }
    if (int32_t(millis() - nextAttempt) < 0) return;
    if (time(nullptr) < 1704067200) {
        Serial.println("Archive waiting for UTC (NTP, GPS or USB).");
        nextAttempt = millis() + 3000;
        return;
    }
    // Empty queues are polled cheaply. A newly persisted event bypasses idle polling.
    if (!batch.count && spool.lastSequence() == idleSequence &&
        int32_t(millis() - nextIdlePoll) < 0) return;
    if (batch.prepare(spool)) {
        prepareTimes();
        idleSequence = UINT64_MAX;
        busy = startBatch();
    } else {
        idleSequence = spool.lastSequence();
        nextIdlePoll = millis() + 250;
    }
}
#endif
