#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
using std::min;
using std::max;
struct SerialStub {
    template<class... T> void printf(const char*, T...) {}
    void println(const char*) {}
};
inline SerialStub Serial;
