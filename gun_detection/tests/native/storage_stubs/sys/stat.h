#pragma once
#include "FS.h"
#include <cerrno>

// Stub the exact VFS path used by the real filesystem's quiet mount-time probe.
#define stat storage_test_stat
struct stat { size_t st_size = 0; };
inline int stat(const char* path, struct stat* info) {
    ++storage_test::statCalls;
    std::string full(path);
    if (full == storage_test::failStatPath) { errno = EIO; return -1; }
    if (full.compare(0, 9, "/littlefs") != 0) { errno = EINVAL; return -1; }
    const auto it = storage_test::files.find(full.substr(9));
    if (it == storage_test::files.end()) { errno = ENOENT; return -1; }
    info->st_size = it->second->size();
    return 0;
}
