/*
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace stackchan::led {
struct Rgb {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const Rgb&) const = default;
};

// A single allocation-free animation engine shared by the Web test and speech.
class RandomFade {
public:
    static constexpr unsigned kCount = 12;
    using Frame = std::array<Rgb, kCount>;
    static constexpr uint32_t kMinMs = 200, kMaxMs = 1500;
    static constexpr uint8_t kMaxRgb = 168;

    void Start(uint32_t now, uint32_t seed, const Frame& initial) {
        rng_ = seed ? seed : 1;
        for (unsigned i = 0; i < kCount; ++i) {
            pixels_[i].from = Clamp(initial[i]);
            frame_[i] = pixels_[i].from;
            Next(pixels_[i], now);
        }
    }

    void Tick(uint32_t now) {
        for (unsigned i = 0; i < kCount; ++i) {
            auto& p = pixels_[i];
            const uint32_t elapsed = now - p.since;
            if (elapsed >= p.duration) {
                frame_[i] = p.to;
                p.from = p.to;
                // Skip expired intervals after a delayed update; never catch up in a burst.
                Next(p, now);
            } else {
                const uint32_t t = elapsed * 1024 / p.duration;
                const uint32_t ease = t * t * (3072 - 2 * t) / (1024 * 1024);
                frame_[i] = {Blend(p.from.r, p.to.r, ease), Blend(p.from.g, p.to.g, ease),
                             Blend(p.from.b, p.to.b, ease)};
            }
        }
    }
    const Frame& Colors() const { return frame_; }

private:
    struct Pixel { Rgb from{}, to{}; uint32_t since = 0, duration = kMinMs; };
    std::array<Pixel, kCount> pixels_{};
    Frame frame_{};
    uint32_t rng_ = 1;
    uint32_t Random() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return rng_;
    }
    static uint8_t Limit(uint8_t v) { return v > kMaxRgb ? kMaxRgb : v; }
    static Rgb Clamp(Rgb c) { return {Limit(c.r), Limit(c.g), Limit(c.b)}; }
    static uint8_t Blend(uint8_t a, uint8_t b, uint32_t t) {
        return static_cast<uint8_t>((a * (1024 - t) + b * t + 512) / 1024);
    }
    void Next(Pixel& p, uint32_t now) {
        p.to = {static_cast<uint8_t>(Random() % (kMaxRgb + 1)),
                static_cast<uint8_t>(Random() % (kMaxRgb + 1)),
                static_cast<uint8_t>(Random() % (kMaxRgb + 1))};
        p.duration = kMinMs + Random() % (kMaxMs - kMinMs + 1);
        p.since = now;
    }
};

// Existing base and conversation layers; sleep blackout remains highest priority.
class Controller {
public:
    static constexpr unsigned kCount = RandomFade::kCount;
    using Frame = RandomFade::Frame;

    void SetBase(unsigned index, Rgb rgb) {
        if (index >= kCount) return;
        std::lock_guard<std::mutex> lock(mutex_);
        base_[index] = rgb;
        // A pending base fade must not erase a speech random effect. Explicit
        // manual LED requests call CancelRandom before taking base ownership.
        if (!conversation_random_) conversation_owned_ = false;
    }

    void SetConversation(Rgb rgb) {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_ = rgb;
        conversation_owned_ = true;
        conversation_random_ = false;
    }

    void SetConversationRandom() {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_owned_ = true;
        conversation_random_ = true;
    }

    void ReleaseConversation() {
        std::lock_guard<std::mutex> lock(mutex_);
        conversation_owned_ = false;
        conversation_random_ = false;
    }

    void SetTestRandom(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        test_random_ = enabled;
        if (!enabled) test_allowed_ = false;
    }

    void CancelRandom() {
        std::lock_guard<std::mutex> lock(mutex_);
        CancelRandomLocked();
    }

    void SetSleepBlackout(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        sleep_blackout_ = enabled;
    }

    // Called from the existing update loop. No new task, per-pixel timer or heap allocation.
    template <typename Seed>
    bool TickRandom(uint32_t now, bool test_allowed, bool shutdown, Seed&& seed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown) CancelRandomLocked();
        test_allowed_ = test_allowed && !shutdown;
        const auto mode = Mode();
        if (mode != active_mode_) {
            active_mode_ = mode;
            if (mode != 0) fade_.Start(now, seed(), applied_);
            last_tick_ = now;
            return true;
        }
        if (mode == 0 || now - last_tick_ < 30) return false;
        last_tick_ = now;
        fade_.Tick(now);
        return true;
    }

    // Preserve the ordinary changed-pixel writes. During animation, send one
    // full frame through the existing expander RAM API instead of 12 I2C writes.
    template <typename Write, typename Refresh, typename WriteFrame>
    void Flush(Write&& write, Refresh&& refresh, WriteFrame&& write_frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto mode = Mode();
        const bool random = mode != 0 && mode == active_mode_;
        Frame next{};
        bool dirty = false;
        for (unsigned i = 0; i < kCount; ++i) {
            next[i] = sleep_blackout_ ? Rgb{} : random ? fade_.Colors()[i] :
                      (conversation_owned_ ? conversation_ : base_[i]);
            if (initialized_ && next[i] == applied_[i]) continue;
            if (!random) write(i, next[i].r, next[i].g, next[i].b);
            dirty = true;
        }
        if (dirty) {
            if (random) write_frame(next);
            refresh();
            applied_ = next;
        }
        initialized_ = true;
    }

private:
    // 0 = existing layers, 1 = manual Web test, 2 = conversational smile.
    unsigned Mode() const {
        if (sleep_blackout_) return 0;
        if (conversation_owned_) return conversation_random_ ? 2 : 0;
        return test_random_ && test_allowed_ ? 1 : 0;
    }
    void CancelRandomLocked() {
        test_random_ = test_allowed_ = false;
        if (conversation_random_) conversation_owned_ = false;
        conversation_random_ = false;
    }
    std::mutex mutex_;
    Frame base_{}, applied_{};
    Rgb conversation_{};
    RandomFade fade_;
    uint32_t last_tick_ = 0;
    unsigned active_mode_ = 0;
    bool conversation_owned_ = false, conversation_random_ = false;
    bool test_random_ = false, test_allowed_ = false;
    bool sleep_blackout_ = false;
    bool initialized_ = false;
};
} // namespace stackchan::led
