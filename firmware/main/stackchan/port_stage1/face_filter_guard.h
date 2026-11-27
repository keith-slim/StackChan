#pragma once
#include <cmath>
namespace stackchan::stage1 {
// EMA reduces jitter but is not allowed to command the old side of center.
inline float GuardedFaceError(float raw, float filtered, float deadband) {
    if (std::abs(raw) < deadband || raw * filtered <= 0.0f) return 0.0f;
    return std::abs(filtered) < deadband ? 0.0f : filtered;
}
inline bool FaceCenterEligible(float raw_x, float raw_y, float filtered_x, float filtered_y) {
    return std::abs(raw_x) < 0.035f && std::abs(raw_y) < 0.05f &&
           std::abs(filtered_x) < 0.035f && std::abs(filtered_y) < 0.05f;
}
inline bool FaceCenterLost(float raw_x, float raw_y, float filtered_x, float filtered_y) {
    return std::abs(raw_x) > 0.075f || std::abs(raw_y) > 0.10f ||
           std::abs(filtered_x) > 0.075f || std::abs(filtered_y) > 0.10f;
}
} // namespace stackchan::stage1
