#pragma once
#include <cstdint>
#include "../event_timer.h"

namespace stackchan::stage1 {
enum class NapReset : uint8_t { None, Config, Night, ClockInvalid, ManualNap, Full };
struct NapNotice {
    NapReset reset = NapReset::None;
    uint32_t scheduled_ms = 0;
    bool deferred = false;
};

// One monotonic deadline, independent of scene/face ownership. A due draw
// stays pending until admission; no catch-up queue or repeated random draws.
class NapSchedule {
public:
    void Arm(uint32_t now, uint32_t delay) {
        if (due_) return;
        due_ = event_timer::Next(now, delay);
        notice_.scheduled_ms = delay;
    }
    bool Armed() const { return due_ != 0; }
    bool Due(uint32_t now) const {
        return deferred_ || event_timer::Due(due_, now);
    }
    void Defer(uint32_t now) {
        if (!deferred_ && Due(now)) {
            deferred_ = true;
            notice_.deferred = true;
        }
    }
    void Consume() {
        due_ = 0;
        deferred_ = false;
    }
    void Reset(NapReset reason) {
        if (Armed()) notice_.reset = reason;
        Consume();
    }
    NapNotice TakeNotice() {
        auto result = notice_;
        notice_ = {};
        return result;
    }
private:
    uint32_t due_ = 0;
    bool deferred_ = false;
    NapNotice notice_{};
};
} // namespace stackchan::stage1
