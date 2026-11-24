#pragma once
#include <cstdint>

// 动作识别配置（普通模式、低功耗模式共用）。修改后需重新编译、烧录。
// 当前安装方向：横向由 Z 轴主导，竖向由 X 轴主导；幅度单位为 ADC counts。
namespace motion_config {
// 两个方向共用的动作分段参数。调整这里会同时影响横向和竖向。
constexpr uint64_t SAMPLE_INTERVAL_US = 50000;  // 50 ms；减小可捕获更短动作。
constexpr uint64_t CALIBRATION_US = 2000000;    // 启动静止校准 2 秒。
constexpr float CALIBRATION_RANGE = 100.0F;    // 校准期间每轴最大波动；增大更容易校准。
constexpr uint64_t MAX_GAP_US = 500000;         // 输入中断超过此值重新校准。
constexpr uint64_t CANDIDATE_US = 200000;       // 两次启动采样允许的最大间隔。
constexpr float START = 100.0F;                // 启动阈值；降低更容易启动，也更易受噪声影响。
constexpr float STOP = 60.0F;                  // 平稳阈值；增大更容易确认结束，必须小于 START。
constexpr uint64_t QUIET_US = 500000;           // 连续平稳 0.5 秒才确认；减小可更早触发。
constexpr uint64_t MAX_DURATION_US = 3000000;   // 超过 3 秒仍未结束则拒绝；增大允许更长动作。
constexpr uint64_t COOLDOWN_US = 1000000;       // 判定后冷却 1 秒；减小允许更密集动作。

struct DirectionSensitivity {
    float minPeak;             // 最小峰值；降低更容易识别小幅动作。
    uint16_t minStrongSamples; // 超过 START 的最少采样点；降低更容易识别短动作（至少 2）。
    float dominance;           // 主轴能量占三轴总能量的最低比例；降低允许更多偏斜。
    float axisRatio;           // 主轴与另一个方向主轴的最低能量比；降低允许更多混合运动。
};

// 两个方向可独立调整；以下默认值保持原来的识别效果。
constexpr DirectionSensitivity HORIZONTAL = {
    200.0F,  // minPeak：横向最小峰值（射击事件）。
    3,       // minStrongSamples：横向最少有效采样点。
    0.65F,   // dominance：Z 轴能量占比。
    3.0F     // axisRatio：Z / X 能量比。
};
constexpr DirectionSensitivity VERTICAL = {
    150.0F,  // minPeak：竖向最小峰值（Wi-Fi 指令）。
    3,       // minStrongSamples：竖向最少有效采样点。
    0.55F,   // dominance：X 轴能量占比。
    2.0F     // axisRatio：X / Z 能量比。
};

constexpr bool valid(DirectionSensitivity p) {
    return p.minPeak >= START && p.minStrongSamples >= 2 &&
           p.dominance > 0.5F && p.dominance <= 1.0F && p.axisRatio > 1.0F;
}
static_assert(valid(HORIZONTAL) && valid(VERTICAL), "Invalid direction sensitivity");
static_assert(START > STOP && STOP > 0 && CALIBRATION_RANGE > 0,
              "Motion thresholds must satisfy START > STOP > 0");
static_assert(SAMPLE_INTERVAL_US > 0 && CALIBRATION_US / SAMPLE_INTERVAL_US >= 19 &&
              CALIBRATION_US / SAMPLE_INTERVAL_US + 2 <= 4096,
              "Calibration requires 20 samples and at most 4096 buffer entries");
static_assert(MAX_GAP_US >= SAMPLE_INTERVAL_US && CANDIDATE_US >= SAMPLE_INTERVAL_US &&
              QUIET_US > 0 && MAX_DURATION_US > QUIET_US && MAX_DURATION_US > CANDIDATE_US,
              "Invalid motion timing parameters");
}
