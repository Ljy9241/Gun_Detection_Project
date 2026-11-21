#pragma once
#include "FS.h"
struct LittleFSStub {
    bool begin(bool, const char* = "/littlefs") { return true; }
    uint32_t totalBytes() const { return 1024 * 1024; }
    uint32_t usedBytes() const {
        uint32_t bytes = 8192;
        for (const auto& item : storage_test::files) bytes += item.second->size();
        return bytes;
    }
    bool exists(const char* path) {
        ++storage_test::existsCalls;
        return static_cast<bool>(open(path, FILE_READ)); // Match Arduino LittleFSImpl.
    }
    File open(const char* path, const char* mode) {
        auto it = storage_test::files.find(path);
        if (std::strcmp(mode, FILE_READ) == 0) {
            ++storage_test::readOpens;
            if (it == storage_test::files.end()) ++storage_test::missingReadOpens;
            return it == storage_test::files.end() ? File{} : File(it->second, false, path);
        }
        auto& data = storage_test::files[path];
        if (!data || std::strcmp(mode, FILE_WRITE) == 0) data = std::make_shared<std::vector<uint8_t>>();
        return File(data, std::strcmp(mode, FILE_APPEND) == 0, path);
    }
    bool remove(const char* path) {
        return !storage_test::failRemove && storage_test::files.erase(path);
    }
};
inline LittleFSStub LittleFS;
