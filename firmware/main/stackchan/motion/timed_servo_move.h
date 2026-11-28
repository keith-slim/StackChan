#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stackchan::motion {

// Wall-clock position interpolation, independent of the spring animation.
// Linear progress starts moving immediately (within actuator resolution).
class TimedServoMove {
public:
    void Start(int from, int to, uint32_t duration_ms, uint32_t now) {
        from_ = from;
        to_ = to;
        duration_ = std::max<uint32_t>(1, duration_ms);
        started_ = now;
        active_ = from != to;
    }

    bool Active() const { return active_; }
    void Cancel() { active_ = false; }

    int Sample(uint32_t now) {
        const uint32_t elapsed = now - started_;
        if (elapsed >= duration_) {
            active_ = false;
            return to_;
        }
        const double progress = static_cast<double>(elapsed) / duration_;
        return static_cast<int>(std::lround(from_ + (static_cast<double>(to_) - from_) * progress));
    }

private:
    bool active_ = false;
    uint32_t started_ = 0, duration_ = 1;
    int from_ = 0, to_ = 0;
};

} // namespace stackchan::motion
