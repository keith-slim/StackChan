#pragma once
#include <algorithm>
#include <cmath>
namespace stackchan::stage1 {
// EMA reduces jitter but is not allowed to command the old side of center.
inline float GuardedFaceError(float raw, float filtered, float deadband) {
    if (std::abs(raw) < deadband || raw * filtered <= 0.0f) return 0.0f;
    return std::abs(filtered) < deadband ? 0.0f : filtered;
}

class FacePitchHold {
    static constexpr float kStop = 0.035f;
    static constexpr float kResume = 0.060f;
    static constexpr float kResumeImmediately = 0.10f;

public:
    void Reset() {
        active_direction_ = 0;
        pending_direction_ = 0;
    }

    float Update(float raw, float filtered) {
        const float error = GuardedFaceError(raw, filtered, kStop);
        if (error == 0.0f) {
            Reset();
            return 0.0f;
        }

        const int direction = error > 0.0f ? 1 : -1;
        if (direction == active_direction_) return error;

        // 中央付近の小さな揺れは、左右位置にかかわらず上下軸で吸収する。
        const bool reversing = active_direction_ != 0;
        active_direction_ = 0;
        const float displacement = std::min(std::abs(raw), std::abs(error));
        if (displacement < kResume) {
            pending_direction_ = 0;
            return 0.0f;
        }
        if (pending_direction_ == direction || (!reversing && displacement >= kResumeImmediately)) {
            active_direction_ = direction;
            pending_direction_ = 0;
            return error;
        }
        pending_direction_ = direction;
        return 0.0f;
    }

private:
    int active_direction_ = 0;
    int pending_direction_ = 0;
};

inline bool FaceCenterEligible(float raw_x, float raw_y, float filtered_x, float filtered_y) {
    return std::abs(raw_x) < 0.035f && std::abs(raw_y) < 0.05f &&
           std::abs(filtered_x) < 0.035f && std::abs(filtered_y) < 0.05f;
}
inline bool FaceCenterLost(float raw_x, float raw_y, float filtered_x, float filtered_y) {
    return std::abs(raw_x) > 0.075f || std::abs(raw_y) > 0.10f ||
           std::abs(filtered_x) > 0.075f || std::abs(filtered_y) > 0.10f;
}
} // namespace stackchan::stage1
