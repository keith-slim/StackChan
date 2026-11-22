#pragma once
#include <cstdint>

namespace stackchan::event_timer {

// Zero is reserved for an unarmed deadline. Intervals must be < 2^31 ms.
inline bool Due(uint32_t deadline, uint32_t now) {
    return deadline != 0 && static_cast<int32_t>(now - deadline) >= 0;
}
inline uint32_t Next(uint32_t now, uint32_t delay) {
    const uint32_t deadline = now + delay;
    return deadline ? deadline : 1U;
}
template<class Delay>
bool DiscardDue(uint32_t& deadline, uint32_t now, Delay delay) {
    if (!Due(deadline, now)) return false;
    deadline = Next(now, delay());
    return true;
}

} // namespace stackchan::event_timer
