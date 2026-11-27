#pragma once
#include <algorithm>
#include <cmath>
namespace stackchan::stage1 {
// EMA reduces jitter but is not allowed to command the old side of center.
inline float GuardedFaceError(float raw, float filtered, float deadband) {
    if (std::abs(raw) < deadband || raw * filtered <= 0.0f) return 0.0f;
    return std::abs(filtered) < deadband ? 0.0f : filtered;
}

inline float NormalizedFaceHeight(float top, float bottom, float image_height) {
    if (!std::isfinite(top) || !std::isfinite(bottom) || !std::isfinite(image_height) ||
        image_height <= 0.0f || bottom <= top) return 0.0f;
    return (std::clamp(bottom, 0.0f, image_height) - std::clamp(top, 0.0f, image_height)) /
           image_height;
}

class FacePitchHold {
    static constexpr float kStop = 0.035f;
    static constexpr float kDefaultResume = 0.060f;
    static constexpr float kMinResume = 0.035f;
    static constexpr float kMaxResume = 0.10f;
    static constexpr float kResumeImmediately = 0.10f;
    static constexpr float kFaceHeightRatio = 0.18f;
    static constexpr float kHeightFilterAlpha = 0.20f;

public:
    void Reset() {
        ResetMotion();
        face_height_ = 0.0f;
    }

    void ResetMotion() {
        active_direction_ = 0;
        pending_direction_ = 0;
    }

    float ResumeBand() const {
        if (face_height_ == 0.0f) return kDefaultResume;
        return std::clamp(face_height_ * kFaceHeightRatio, kMinResume, kMaxResume);
    }

    float Update(float raw, float filtered, float face_height = 0.0f) {
        // 初回の顔の大きさで保持幅を決め、その後の枠の揺れはゆっくり反映する。
        if (face_height > 0.0f && face_height <= 1.0f) {
            face_height_ = face_height_ == 0.0f ? face_height :
                face_height_ + (face_height - face_height_) * kHeightFilterAlpha;
        }
        const float resume = ResumeBand();
        const float scale = resume / kDefaultResume;
        const float error = GuardedFaceError(raw, filtered, kStop * scale);
        if (error == 0.0f) {
            ResetMotion();
            return 0.0f;
        }

        const int direction = error > 0.0f ? 1 : -1;
        if (direction == active_direction_) return error;

        // 中央付近の小さな揺れは、左右位置にかかわらず上下軸で吸収する。
        const bool reversing = active_direction_ != 0;
        active_direction_ = 0;
        const float displacement = std::min(std::abs(raw), std::abs(error));
        if (displacement < resume) {
            pending_direction_ = 0;
            return 0.0f;
        }
        const float immediate = kResumeImmediately * std::max(1.0f, scale);
        if (pending_direction_ == direction || (!reversing && displacement >= immediate)) {
            active_direction_ = direction;
            pending_direction_ = 0;
            return error;
        }
        pending_direction_ = direction;
        return 0.0f;
    }

private:
    float face_height_ = 0.0f;
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
