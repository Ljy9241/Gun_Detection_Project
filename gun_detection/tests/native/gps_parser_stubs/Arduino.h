#pragma once
#include <cstdint>
#include <cmath>
using byte = uint8_t;
inline unsigned long millis() { return 1000; }
constexpr double TWO_PI = 6.28318530717958647692;
inline double radians(double value) { return value * TWO_PI / 360.0; }
inline double degrees(double value) { return value * 360.0 / TWO_PI; }
inline double sq(double value) { return value * value; }
