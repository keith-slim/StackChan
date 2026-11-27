#pragma once
#include <cstdint>
#include <mutex>

namespace stackchan::stage1 {
inline constexpr uint32_t kPerceptionMaxAgeMs = 750;
inline bool IsPerceptionFresh(uint32_t now, uint32_t captured) {
    return now - captured <= kPerceptionMaxAgeMs;
}
struct PerceptionLook {
    int yaw = 0, pitch = 0;
    bool face = false;
    uint32_t captured = 0, generation = 0;
};
// Latest-wins mailbox: one coherent payload, no heap queue, and no lock is
// held across UART, HTTP, LVGL, or any other blocking operation.
class PerceptionMailbox {
public:
    enum class Result { Empty, Waiting, Expired, Ready };
    uint32_t Generation() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generation_;
    }
    void Invalidate() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        pending_ = false;
    }
    bool Publish(const PerceptionLook& value, uint32_t now) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (value.generation != generation_ || !IsPerceptionFresh(now, value.captured)) return false;
        if (pending_ && static_cast<int32_t>(value.captured - value_.captured) < 0) return false;
        value_ = value;
        pending_ = true;
        return true;
    }
    bool Pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pending_;
    }
    Result Take(uint32_t now, bool available, PerceptionLook& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_) return Result::Empty;
        // The consumer may have sampled now immediately before publication.
        if (static_cast<int32_t>(now - value_.captured) < 0) return Result::Waiting;
        if (!IsPerceptionFresh(now, value_.captured)) {
            pending_ = false;
            return Result::Expired;
        }
        if (!available) return Result::Waiting;
        out = value_;
        pending_ = false;
        return Result::Ready;
    }
private:
    mutable std::mutex mutex_;
    PerceptionLook value_{};
    uint32_t generation_ = 1;
    bool pending_ = false;
};
} // namespace stackchan::stage1
