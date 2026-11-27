#pragma once
#include <atomic>
#include <cstdint>

namespace stackchan::low_battery {
inline constexpr int kShutdownLevel = 10;
inline constexpr int kWarningLevel = 25;
inline constexpr uint32_t kAdmissionTimeoutMs = 15000;
inline constexpr uint32_t kSpeechTimeoutMs = 45000;
inline constexpr uint32_t kGraceMs = 30000;
inline constexpr char kPhrase[] = "もうダメ～　落ちる～";

struct Sample {
    int level = -1;
    bool valid = false;
    bool present = false;
    bool external = false;
    bool charging = false;
    bool discharging = false;
};
constexpr bool CanPowerOff(Sample s) {
    return s.valid && s.present && !s.external && !s.charging && s.discharging &&
           s.level >= 0 && s.level <= 100;
}
struct Detector {
    unsigned consecutive = 0;
    bool active = false;
    bool Observe(Sample s) {
        if (!s.valid) { consecutive = 0; return active; }
        if (s.external || !s.present) { consecutive = 0; active = false; return false; }
        // Once admitted, latch the scene until external power returns.
        // Load removal can make the gauge rebound during the farewell.
        if (active) return true;
        if (CanPowerOff(s) && s.level <= kShutdownLevel) {
            if (++consecutive >= 3) active = true;
        } else consecutive = 0;
        return active;
    }
};
enum class Phase { Admission, Speech, Grace, Ready };
struct Sequence {
    Phase phase = Phase::Admission;
    uint32_t since = 0;
    void Begin(uint32_t now) { phase = Phase::Admission; since = now; }
    void Queued(uint32_t now) { phase = Phase::Speech; since = now; }
    bool Tick(uint32_t now, bool busy, bool pcm_idle) {
        if ((phase == Phase::Admission && now - since >= kAdmissionTimeoutMs) ||
            (phase == Phase::Speech && ((!busy && pcm_idle) || now - since >= kSpeechTimeoutMs))) {
            phase = Phase::Grace;
            since = now;
        }
        if (phase == Phase::Grace && now - since >= kGraceMs) phase = Phase::Ready;
        return phase == Phase::Ready;
    }
};
// Only the board's existing 1s power poll publishes samples / detector state.
// Stage1 owns the visual sequence and publishes the final power-off request.
inline std::atomic<int> sample_bits{-1};
// Odd epochs are active; even epochs are inactive. A delayed UI callback
// from a canceled farewell cannot authorize shutdown for a later one.
inline std::atomic<uint32_t> epoch{0};
inline std::atomic<uint32_t> ready_epoch{0};
inline uint32_t Epoch() { return epoch.load(); }
inline bool Active() { return (Epoch() & 1U) != 0; }
inline bool Ready() {
    const uint32_t current = Epoch();
    return (current & 1U) && ready_epoch.load() == current;
}
inline void MarkReady(uint32_t current) {
    if ((current & 1U) && current == Epoch()) ready_epoch.store(current);
}
inline void Publish(Sample s, bool shutdown) {
    sample_bits.store(s.valid && s.present
        ? s.level | (s.charging ? 128 : 0) | (s.external ? 256 : 0) : -1);
    const uint32_t current = Epoch();
    if (bool(current & 1U) != shutdown) epoch.store(current + 1U);
}
} // namespace stackchan::low_battery
