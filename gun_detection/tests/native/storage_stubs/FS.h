#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
enum SeekMode { SeekSet };
namespace storage_test {
inline std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
inline bool failRead = false, failRemove = false;
inline size_t shortWrite = SIZE_MAX;
inline unsigned missingReadOpens = 0;
inline std::string failReadPath;
inline std::string failStatPath;
inline unsigned statCalls = 0, readOpens = 0, existsCalls = 0;
}
namespace fs {
class File {
    std::shared_ptr<std::vector<uint8_t>> data_;
    size_t pos_ = 0;
    std::string path_;
public:
    File() = default;
    File(std::shared_ptr<std::vector<uint8_t>> data, bool append, std::string path):
        data_(data), pos_(append ? data->size() : 0), path_(path) {}
    explicit operator bool() const { return bool(data_); }
    size_t size() const { return data_ ? data_->size() : 0; }
    size_t position() const { return pos_; }
    bool seek(size_t offset, SeekMode) {
        if (!data_ || offset > size()) return false;
        pos_ = offset;
        return true;
    }
    size_t read(uint8_t* out, size_t count) {
        if (!data_ || storage_test::failRead || storage_test::failReadPath == path_) return 0;
        count = std::min(count, size() - pos_);
        std::memcpy(out, data_->data() + pos_, count);
        pos_ += count;
        return count;
    }
    size_t write(const uint8_t* in, size_t count) {
        if (!data_) return 0;
        count = std::min(count, storage_test::shortWrite);
        data_->resize(std::max(size(), pos_ + count));
        std::memcpy(data_->data() + pos_, in, count);
        pos_ += count;
        return count;
    }
    void close() { data_.reset(); }
};
}
using File = fs::File;
