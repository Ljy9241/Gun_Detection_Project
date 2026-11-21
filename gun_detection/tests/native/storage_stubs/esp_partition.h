#pragma once
#include <cstdint>
constexpr int ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_SPIFFS = 130;
struct esp_partition_t { uint32_t size; };
inline const esp_partition_t* esp_partition_find_first(int, int, const char*) {
    static esp_partition_t partition{1024 * 1024};
    return &partition;
}
