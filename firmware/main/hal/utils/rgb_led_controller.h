/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>
#include <mutex>

// One synchronized state for the 12 physical neck LEDs. Ordinary NeonLight
// animations write the base layer. Conversation owns LED 0 only until another
// base writer takes it over; sleep blackout overrides every layer.
namespace stackchan::led {
struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb&) const = default;
};

class Controller {
public:
    static constexpr unsigned kCount = 12;

    void SetBase(unsigned index, Rgb rgb) {
        if (index >= kCount) return;
        std::lock_guard<std::mutex> lock(mutex_);
        base_[index] = rgb;
        if (index == 0) conversation_owned_ = false;
    }

    void SetConversation(Rgb rgb) {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_ = rgb;
        conversation_owned_ = true;
    }

    void ReleaseConversation() {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_owned_ = false;
    }

    void SetSleepBlackout(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        sleep_blackout_ = enabled;
    }

    // Keep the lock through the hardware flush so concurrent writers cannot
    // interleave partial frames. Send only LEDs whose effective RGB changed.
    template <typename Write, typename Refresh>
    void Flush(Write&& write, Refresh&& refresh) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool dirty = false;
        for (unsigned i = 0; i < kCount; ++i) {
            const Rgb rgb = sleep_blackout_ ? Rgb{} :
                (i == 0 && conversation_owned_ ? conversation_ : base_[i]);
            if (initialized_ && rgb == applied_[i]) continue;
            write(i, rgb.r, rgb.g, rgb.b);
            applied_[i] = rgb;
            dirty = true;
        }
        if (dirty) refresh();
        initialized_ = true;
    }

private:
    std::mutex mutex_;
    std::array<Rgb, kCount> base_{};
    std::array<Rgb, kCount> applied_{};
    Rgb conversation_{};
    bool conversation_owned_ = false;
    bool sleep_blackout_ = false;
    bool initialized_ = false;
};
} // namespace stackchan::led
