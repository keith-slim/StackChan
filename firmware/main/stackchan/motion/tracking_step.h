#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace stackchan::motion {
// All positions and rates are tenths of a degree and tenths of a degree/s.
inline constexpr uint32_t kTrackingStepMs = 20;
inline constexpr uint32_t kTrackingReplanMs = 3 * kTrackingStepMs;
struct TrackingStepResult { float position; float velocity; bool reached; };
inline TrackingStepResult AdvanceTracking(float position, float velocity, int goal,
                                           int minimum, int maximum, float rate,
                                           float stiffness, float damping, uint32_t elapsed) {
    goal = std::clamp(goal, minimum, maximum);
    position = std::clamp(position, static_cast<float>(minimum), static_cast<float>(maximum));
    velocity = std::clamp(velocity, -rate, rate);
    // The caller replans from feedback after a stalled control cycle. Never
    // integrate an unbounded backlog of physical time into one command.
    elapsed = std::min(elapsed, kTrackingReplanMs);
    const float acceleration_limit = rate * damping;
    while (elapsed) {
        const uint32_t step = std::min(elapsed, kTrackingStepMs);
        elapsed -= step;
        const float dt = step * 0.001f;
        const float error = goal - position;
        const float acceleration = std::clamp(stiffness * error - damping * velocity,
                                              -acceleration_limit, acceleration_limit);
        velocity = std::clamp(velocity + acceleration * dt, -rate, rate);
        const float next = position + velocity * dt;
        if ((error >= 0.0f && next >= goal) || (error <= 0.0f && next <= goal) ||
            (std::abs(error) <= 0.1f && std::abs(velocity) <= 0.1f))
            return {static_cast<float>(goal), 0.0f, true};
        position = std::clamp(next, static_cast<float>(minimum), static_cast<float>(maximum));
        if ((position <= minimum && velocity < 0) || (position >= maximum && velocity > 0)) velocity = 0;
    }
    return {position, velocity, false};
}
} // namespace stackchan::motion
