#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
inline esp_err_t esp_littlefs_format(const char*) { return ESP_OK; }
inline const char* esp_err_to_name(int) { return "test"; }
