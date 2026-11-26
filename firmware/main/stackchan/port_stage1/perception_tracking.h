#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stackchan::stage1::tracking {

// Samples are physical feedback, never the animated position.
constexpr uint32_t kCapturePoseSkewMs = 80;
constexpr uint32_t kFaceFrameMaxAgeMs = 750;
constexpr float kYawGain = 500.0f, kPitchGain = 360.0f;
constexpr float kYawDeadband = 0.025f, kPitchDeadband = 0.035f;

struct AxisPose {
    int angle = 0; // 0.1 degrees
    uint32_t sampled_at = 0;
    bool valid = false;
};
struct PerceptionPose {
    AxisPose yaw, pitch;
    uint32_t captured_at = 0;
    uint32_t epoch = 0;
};

// The two clocks are the same uint32_t HAL millisecond clock, including wrap.
inline bool WithinSkew(uint32_t a, uint32_t b, uint32_t maximum) {
    return a - b <= maximum || b - a <= maximum;
}

// Transform the previous image-space estimate by measured head displacement
// before filtering. signed_gain is positive for yaw, negative for pitch.
// Increase responsiveness continuously with actual travel, not a mode switch.
inline float FilterCoordinate(float previous, float detected, int angle_change, float signed_gain) {
    const float predicted = std::clamp(previous - angle_change / signed_gain, 0.0f, 1.0f);
    const float weight = std::clamp(std::abs(static_cast<float>(angle_change)) / 50.0f, 0.0f, 1.0f);
    const float alpha = 0.35f + 0.65f * weight;
    const float filtered = predicted + (detected - predicted) * alpha;
    // Never keep driving toward a center that the latest image has passed.
    // Brake first rather than abruptly commanding the opposite correction.
    if ((detected - 0.5f) * (filtered - 0.5f) < 0.0f) return 0.5f;
    return filtered;
}

inline int FaceDelta(float raw, float filtered, float signed_gain, float deadband, int limit, bool locked) {
    if (locked || std::abs(raw - 0.5f) < deadband || std::abs(filtered - 0.5f) < deadband ||
        (raw - 0.5f) * (filtered - 0.5f) < 0.0f) return 0;
    return static_cast<int>(std::clamp((filtered - 0.5f) * signed_gain,
                                       -static_cast<float>(limit), static_cast<float>(limit)));
}

inline int CaptureTarget(int capture_angle, int delta, int lower, int upper) {
    return std::clamp(capture_angle + delta, lower, upper);
}

} // namespace stackchan::stage1::tracking
