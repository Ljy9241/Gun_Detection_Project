#pragma once
#include <cstdint>
#include <map>
#include <string>
#include "FS.h"
namespace storage_test {
inline std::map<std::string, uint64_t> nvs;
inline std::string failKey;
inline std::string readFailureAfterAck;
inline unsigned ackWrites = 0;
}
class Preferences {
    template<class T> T get(const char* key, T fallback) {
        auto it = storage_test::nvs.find(key);
        return it == storage_test::nvs.end() ? fallback : T(it->second);
    }
    template<class T> size_t put(const char* key, T value) {
        if (storage_test::failKey == key) return 0;
        if (std::string(key) == "acked") {
            ++storage_test::ackWrites;
            storage_test::failReadPath = storage_test::readFailureAfterAck;
        }
        storage_test::nvs[key] = value;
        return sizeof(T);
    }
public:
    bool begin(const char*, bool) { return true; }
    uint64_t getULong64(const char* key, uint64_t fallback) { return get(key, fallback); }
    uint32_t getUInt(const char* key, uint32_t fallback) { return get(key, fallback); }
    bool getBool(const char* key, bool fallback) { return get(key, fallback); }
    size_t putULong64(const char* key, uint64_t value) { return put(key, value); }
    size_t putUInt(const char* key, uint32_t value) { return put(key, value); }
    size_t putBool(const char* key, bool value) { return put(key, value); }
};
