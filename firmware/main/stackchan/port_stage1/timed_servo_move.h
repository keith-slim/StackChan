#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <stackchan/motion/motion.h>

namespace stackchan::stage1 {

// 汎用の時間指定サーボ移動。小さな目標角を順次与え、既存の制御系を再利用する。
class TimedServoMove {
public:
    void Start(motion::Motion& motion, int target_yaw, int target_pitch,
               uint32_t duration_ms, uint32_t now)
    {
        start_yaw_       = motion.getCurrentYawAngle();
        start_pitch_     = motion.getCurrentPitchAngle();
        target_yaw_      = target_yaw;
        target_pitch_    = target_pitch;
        duration_ms_     = std::max<uint32_t>(duration_ms, 1);
        started_at_      = now;
        last_yaw_        = start_yaw_;
        last_pitch_      = start_pitch_;
        active_          = true;
    }

    void Cancel() { active_ = false; }
    bool Active() const { return active_; }

    void Tick(motion::Motion& motion, uint32_t now)
    {
        if (!active_) return;
        const uint32_t elapsed = now - started_at_;
        const float linear = std::min(1.0f, static_cast<float>(elapsed) / duration_ms_);
        // Smoothstepで開始・停止付近を緩やかにする。
        const float progress = linear * linear * (3.0f - 2.0f * linear);
        const int yaw = static_cast<int>(std::lround(start_yaw_ + (target_yaw_ - start_yaw_) * progress));
        const int pitch = static_cast<int>(std::lround(start_pitch_ + (target_pitch_ - start_pitch_) * progress));
        if (yaw != last_yaw_ || pitch != last_pitch_) {
            motion.moveWithSpeed(yaw, pitch, 1000);
            last_yaw_   = yaw;
            last_pitch_ = pitch;
        }
        if (linear >= 1.0f) active_ = false;
    }

private:
    bool active_ = false;
    uint32_t started_at_ = 0;
    uint32_t duration_ms_ = 1;
    int start_yaw_ = 0, start_pitch_ = 0;
    int target_yaw_ = 0, target_pitch_ = 0;
    int last_yaw_ = 0, last_pitch_ = 0;
};

}  // namespace stackchan::stage1
