#pragma once
// Shared S3-style PCM amplitude -> mouth opening. No PCM buffers or tasks.
#include <atomic>
#include <cstdint>
namespace stackchan::speech_mouth {
constexpr unsigned MIN_PERCENT=30, MAX_PERCENT=100, DEFAULT_PERCENT=100;
inline std::atomic<uint8_t> percent{DEFAULT_PERCENT};
inline std::atomic<uint32_t> average{0}, updated_at_ms{0};
inline std::atomic<bool> playing{false};
inline bool SetPercent(unsigned v) {
    if (v < MIN_PERCENT || v > MAX_PERCENT) return false;
    percent.store(static_cast<uint8_t>(v), std::memory_order_relaxed);
    return true;
}
inline uint8_t GetPercent() { return percent.load(std::memory_order_relaxed); }
inline bool Playing() { return playing.load(std::memory_order_relaxed); }
inline void SetPlaying(bool v) {
    average.store(0, std::memory_order_relaxed);
    updated_at_ms.store(0, std::memory_order_relaxed);
    playing.store(v, std::memory_order_relaxed);
}
inline void Observe(uint32_t pcm_average, uint32_t now_ms) {
    if (!Playing()) return;
    average.store(pcm_average, std::memory_order_relaxed);
    updated_at_ms.store(now_ms, std::memory_order_relaxed);
}
inline uint8_t Opening(uint32_t now_ms) {
    if (!Playing()) return 0;
    const uint32_t t = updated_at_ms.load(std::memory_order_relaxed);
    if (!t || static_cast<uint32_t>(now_ms-t)>250) return 0;
    // Same S3 curve: PCM average * size percent / 5000, clamped to 100.
    const uint64_t scaled=static_cast<uint64_t>(average.load(std::memory_order_relaxed))*GetPercent()/5000;
    return static_cast<uint8_t>(scaled>=100?100:scaled);
}
} // namespace stackchan::speech_mouth
