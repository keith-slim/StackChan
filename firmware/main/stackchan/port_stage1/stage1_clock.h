#pragma once
// StackChan stage 1: deterministic, hardware-independent daily / nap / theatre state.
// All times are monotonic milliseconds; local time is used only for day/night boundaries.
#include <cstdint>

namespace stackchan::stage1 {

enum class Phase : uint8_t {
    Boot, Awake, Theatre, NapDrowsy, NapYawn, NapSleep, NapWake,
    NightPending, NightDrowsy, NightYawn, NightSleep, NightTemporary, MorningWake
};
enum class Kind : uint8_t { None, Sneeze, Hiccup, Hungry, Stretch, Idea, Shy, Yawn };
struct Config {
    uint16_t wake_minute = 6 * 60;
    uint16_t sleep_minute = 23 * 60;
    bool theatre = true;
    bool naps = true;
    uint8_t nap_chance = 30;
};
struct Inputs {
    uint32_t ms;
    uint16_t minute;
    bool clock_valid;
    bool standby;
    bool activity;
    uint32_t random;
};
struct State {
    Phase phase = Phase::Boot;
    Kind kind = Kind::None;
    // 0..8 are the nine source dream motifs; -1 means not currently visible.
    int8_t dream = -1;
    uint32_t phase_elapsed_ms = 0;
    bool day = true;
};

class Clock {
public:
    bool SetConfig(const Config& cfg) {
        if (cfg.wake_minute >= 1440 || cfg.sleep_minute >= 1440 ||
            cfg.wake_minute == cfg.sleep_minute || cfg.nap_chance > 100 ||
            cfg.nap_chance % 5 != 0) return false;
        cfg_ = cfg;
        idle_due_ = nap_due_ = 0;
        return true;
    }
    const Config& GetConfig() const { return cfg_; }
    void Reset() {
        phase_ = Phase::Boot;
        kind_ = Kind::None;
        dream_ = -1;
        idle_due_ = nap_due_ = phase_at_ = dream_due_ = dream_hide_ = 0;
        last_kind_ = Kind::None;
    }
    static bool IsDay(uint16_t minute, const Config& c) {
        if (minute >= 1440) return false;
        return c.wake_minute < c.sleep_minute
            ? (minute >= c.wake_minute && minute < c.sleep_minute)
            : (minute >= c.wake_minute || minute < c.sleep_minute);
    }
    State Step(const Inputs& in) {
        if (!in.clock_valid || in.minute >= 1440) {
            Reset();
            return Snapshot(in.ms, true);
        }
        const bool day = IsDay(in.minute, cfg_);
        if (phase_ == Phase::Boot) {
            Enter(day ? Phase::Awake :
                  (in.standby && !in.activity ? Phase::NightSleep : Phase::NightTemporary), in.ms);
            return Snapshot(in.ms, day); // initial sync is silent
        }
        if (!day) {
            if (phase_ != Phase::NightPending && phase_ != Phase::NightDrowsy &&
                phase_ != Phase::NightYawn && phase_ != Phase::NightSleep &&
                phase_ != Phase::NightTemporary) {
                Enter(Phase::NightPending, in.ms);
            }
            if (phase_ == Phase::NightPending && in.standby && !in.activity)
                Enter(Phase::NightDrowsy, in.ms);
            else if (phase_ == Phase::NightDrowsy) {
                if (!in.standby || in.activity) Enter(Phase::NightPending, in.ms);
                else if (Elapsed(in.ms) >= 10000) Enter(Phase::NightYawn, in.ms);
            } else if (phase_ == Phase::NightYawn) {
                if (!in.standby || in.activity) Enter(Phase::NightPending, in.ms);
                else if (Elapsed(in.ms) >= 3500) Enter(Phase::NightSleep, in.ms);
            } else if (phase_ == Phase::NightSleep && (!in.standby || in.activity)) {
                Enter(Phase::NightTemporary, in.ms);
            } else if (phase_ == Phase::NightTemporary) {
                if (!in.standby || in.activity) phase_at_ = in.ms;
                else if (Elapsed(in.ms) >= 15000) Enter(Phase::NightSleep, in.ms);
            }
            return Snapshot(in.ms, day);
        }
        if (phase_ == Phase::NightSleep || phase_ == Phase::NightTemporary ||
            phase_ == Phase::NightPending || phase_ == Phase::NightDrowsy ||
            phase_ == Phase::NightYawn) {
            Enter(in.standby && !in.activity ? Phase::MorningWake : Phase::Awake, in.ms);
            return Snapshot(in.ms, day);
        }
        if (!in.standby || in.activity) {
            if (phase_ != Phase::Awake) Enter(Phase::Awake, in.ms);
            idle_due_ = nap_due_ = 0;
            return Snapshot(in.ms, day);
        }
        switch (phase_) {
        case Phase::Theatre:
            if (Elapsed(in.ms) >= TheatreDuration(kind_)) Enter(Phase::Awake, in.ms);
            break;
        case Phase::NapDrowsy:
            if (Elapsed(in.ms) >= 26500) Enter(Phase::NapYawn, in.ms);
            break;
        case Phase::NapYawn:
            if (Elapsed(in.ms) >= 3500) {
                Enter(Phase::NapSleep, in.ms);
                nap_sleep_ms_ = 5 * 60000 + in.random % (10 * 60000 + 1);
                dream_due_ = in.ms + DreamWait(in.random >> 4);
            }
            break;
        case Phase::NapSleep:
            if (Elapsed(in.ms) >= nap_sleep_ms_) Enter(Phase::NapWake, in.ms);
            else {
                if (dream_ >= 0 && static_cast<int32_t>(in.ms - dream_hide_) >= 0) dream_ = -1;
                if (static_cast<int32_t>(in.ms - dream_due_) >= 0) {
                    dream_ = static_cast<int8_t>(in.random % 9);
                    dream_hide_ = in.ms + 10000;
                    dream_due_ = in.ms + DreamWait(in.random >> 8);
                }
            }
            break;
        case Phase::NapWake:
            if (Elapsed(in.ms) >= 2800) Enter(Phase::Awake, in.ms);
            break;
        case Phase::MorningWake:
            if (Elapsed(in.ms) >= 2800) Enter(Phase::Awake, in.ms);
            break;
        case Phase::Awake:
            if (cfg_.theatre) {
                if (!idle_due_) idle_due_ = in.ms + 60000 + in.random % 120001;
                if (static_cast<int32_t>(in.ms - idle_due_) >= 0) {
                    const auto last = static_cast<uint8_t>(last_kind_);
                    const auto slot = static_cast<uint8_t>(in.random % (last ? 6 : 7));
                    const auto index = static_cast<uint8_t>(slot + 1 + (last && slot >= last - 1 ? 1 : 0));
                    kind_ = static_cast<Kind>(index);
                    last_kind_ = kind_;
                    Enter(Phase::Theatre, in.ms);
                    idle_due_ = 0;
                    return Snapshot(in.ms, day);
                }
            }
            if (cfg_.naps) {
                if (!nap_due_) nap_due_ = in.ms + 10 * 60000 + in.random % (10 * 60000 + 1);
                if (static_cast<int32_t>(in.ms - nap_due_) >= 0) {
                    nap_due_ = 0;
                    if ((in.random % 100) < cfg_.nap_chance) Enter(Phase::NapDrowsy, in.ms);
                }
            }
            break;
        default: break;
        }
        return Snapshot(in.ms, day);
    }
    static uint32_t TheatreDuration(Kind kind) {
        switch (kind) {
        case Kind::Hungry: return 4000;
        case Kind::Stretch: return 3600;
        case Kind::Yawn: return 3500;
        default: return 2800;
        }
    }
private:
    static uint32_t DreamWait(uint32_t r) { return 15000 + r % 45001; }
    void Enter(Phase phase, uint32_t now) {
        const Phase old = phase_;
        phase_ = phase;
        phase_at_ = now;
        kind_ = phase == Phase::Theatre ? kind_ : Kind::None;
        dream_ = -1;
        if (phase != Phase::Awake) idle_due_ = 0;
        if (phase == Phase::Awake) {
            idle_due_ = 0;
            // A theatre performance must not postpone a nap draw indefinitely.
            if (old != Phase::Theatre) nap_due_ = 0;
        }
    }
    uint32_t Elapsed(uint32_t now) const { return now - phase_at_; }
    State Snapshot(uint32_t now, bool day) const {
        return {phase_, kind_, dream_, now - phase_at_, day};
    }
    Config cfg_{};
    Phase phase_ = Phase::Boot;
    Kind kind_ = Kind::None;
    Kind last_kind_ = Kind::None;
    int8_t dream_ = -1;
    uint32_t phase_at_ = 0, idle_due_ = 0, nap_due_ = 0;
    uint32_t nap_sleep_ms_ = 0, dream_due_ = 0, dream_hide_ = 0;
};

} // namespace stackchan::stage1
