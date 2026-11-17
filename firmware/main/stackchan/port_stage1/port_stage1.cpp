// Stage-1 orchestration. No Gemini/Web/NVS protocol changes in this phase.
#include "port_stage1.h"
#include "stage1_clock.h"
#include "rest_face.h"
#include "dream_face.h"
#include "dream_context.h"
#include <stackchan/stackchan.h>
#include <hal/hal.h>
#include <lvgl.h>
#include <esp_random.h>
#include <atomic>
#include <ctime>
#include <cstring>
#include <memory>

namespace stackchan::stage1 {
namespace {
std::atomic<bool> enabled{false};
std::atomic<bool> idle{false};
std::atomic<bool> interrupted{false};
std::atomic<bool> manual_motion{false};
std::atomic<bool> manual_led{false};
std::atomic<bool> owns_auto{false};
std::atomic<int> queued_cue{-1};

enum class Cue : int { Nod, ShakeHead, TiltHead, Happy, Surprise, Shy, Count };
const char* const cue_names[] = {"nod", "shake_head", "tilt_head", "happy_move", "surprise_move", "shy_move"};

struct Runtime {
    Clock schedule;
    Phase phase = Phase::Boot;
    Kind kind = Kind::None;
    uint32_t phase_at = 0;
    uint32_t last_draw_at = 0;
    bool motion_held = false;
    bool led_held = false;
    bool own_face = false;
    bool own_led = false;
    bool own_neck = false;
    bool cue_active = false;
    Cue cue = Cue::Nod;
    uint32_t cue_at = 0;
    bool cue_second = false;
    uitk::Vector2i saved_angles{0, 0};
    stackchan::avatar::Emotion saved_emotion = stackchan::avatar::Emotion::Neutral;
    std::unique_ptr<AvatarRestFace> rest;
    std::unique_ptr<AvatarDreamFace> dream;

    static bool Sleeping(Phase p) {
        return p == Phase::NapSleep || p == Phase::NightSleep;
    }
    static bool Auto(Phase p) {
        return p == Phase::Theatre || p == Phase::NapDrowsy || p == Phase::NapYawn ||
               p == Phase::NapSleep || p == Phase::NapWake || p == Phase::NightDrowsy ||
               p == Phase::NightYawn || p == Phase::NightSleep || p == Phase::MorningWake;
    }
    void ClearVisuals() {
        last_draw_at = 0;
        dream.reset();
        rest.reset();
        avatar_dream::End();
    }
    void Release(StackChan& sc, bool restore) {
        ClearVisuals();
        if (own_face && restore) sc.avatar().setEmotion(saved_emotion);
        if (own_led && !led_held && restore) {
            sc.leftNeonLight().setColor(0,0,0);
            sc.rightNeonLight().setColor(0,0,0);
        }
        // Only return an automatic posture when no external command took over.
        if (own_neck && !motion_held && restore)
            sc.motion().moveWithSpeed(saved_angles.x, saved_angles.y, 120);
        own_face = own_led = own_neck = false;
        owns_auto.store(false);
    }
    void Begin(StackChan& sc, Phase next, Kind next_kind, uint32_t now) {
        if (phase == next && kind == next_kind) return;
        // Preserve the pre-automation face/pose across a multi-step night/nap scene.
        const bool continuing = own_face && Auto(next);
        if (continuing) ClearVisuals();
        else Release(sc, true);
        phase = next;
        kind = next_kind;
        phase_at = now;
        if (!Auto(next)) return;
        if (!continuing) saved_emotion = sc.avatar().getEmotion();
        own_face = true;
        owns_auto.store(true);
        auto& av = sc.avatar();
        auto& motion = sc.motion();
        // Autonomous motion never overrides a retained explicit neck position.
        auto move = [&](int yaw, int pitch, int speed) {
            if (motion_held) return;
            if (!own_neck) { saved_angles = motion.getCurrentAngles(); own_neck = true; }
            motion.moveWithSpeed(yaw, pitch, speed);
        };
        auto color = [&](uint8_t r,uint8_t g,uint8_t b) {
            if (led_held) return;
            own_led = true;
            sc.leftNeonLight().setColor(r,g,b);
            sc.rightNeonLight().setColor(r,g,b);
        };
        if (next == Phase::Theatre) {
            switch(next_kind) {
            case Kind::Sneeze: av.setEmotion(avatar::Emotion::Doubt); move(0,30,250); color(55,55,75); break;
            case Kind::Hiccup: av.setEmotion(avatar::Emotion::Doubt); move(0,100,200); color(60,50,30); break;
            case Kind::Hungry: av.setEmotion(avatar::Emotion::Sad); move(0,50,120); color(45,25,10); break;
            case Kind::Stretch: av.setEmotion(avatar::Emotion::Sleepy); move(0,220,120); color(70,60,40); break;
            case Kind::Idea: av.setEmotion(avatar::Emotion::Happy); move(50,210,160); color(100,90,20); break;
            case Kind::Shy: av.setEmotion(avatar::Emotion::Happy); move(110,160,130); color(100,30,60); break;
            case Kind::Yawn: av.setEmotion(avatar::Emotion::Sleepy); color(45,45,65); break;
            default: break;
            }
        } else if (next == Phase::NapDrowsy || next == Phase::NightDrowsy) {
            av.setEmotion(avatar::Emotion::Sleepy);
            color(28,18,20);
        } else if (next == Phase::NapYawn || next == Phase::NightYawn) {
            av.setEmotion(avatar::Emotion::Sleepy);
        } else if (Sleeping(next)) {
            av.setEmotion(avatar::Emotion::Sleepy);
            if (!motion_held) move(0,30,100);
            color(10,8,15);
            if (next == Phase::NapSleep) avatar_dream::Begin();
        } else if (next == Phase::NapWake || next == Phase::MorningWake) {
            av.setEmotion(avatar::Emotion::Happy);
            move(0,160,120);
            color(65,48,28);
        }
    }
    void Draw(const State& s) {
        const bool asleep = Sleeping(s.phase);
        const bool yawn = s.phase == Phase::NapYawn || s.phase == Phase::NightYawn ||
                          (s.phase == Phase::Theatre && s.kind == Kind::Yawn);
        if (asleep || yawn) {
            if (!rest) rest = std::make_unique<AvatarRestFace>(lv_screen_active());
            rest->Render(asleep ? AvatarRestPose::Sleeping : AvatarRestPose::Yawning,
                         false, 0, s.phase_elapsed_ms);
        } else rest.reset();
        if (s.phase == Phase::NapSleep || (s.phase == Phase::Theatre &&
             (s.kind == Kind::Hungry || s.kind == Kind::Idea))) {
            if (!dream) dream = std::make_unique<AvatarDreamFace>(lv_screen_active());
            if (s.phase == Phase::NapSleep) dream->Render(s.phase_elapsed_ms);
            else if (s.kind == Kind::Hungry) dream->RenderMotif(2);
            else dream->RenderIdeaBulb(s.phase_elapsed_ms);
        } else dream.reset();
    }
    void BeginCue(StackChan& sc, Cue request, uint32_t now) {
        Release(sc, false);
        cue = request;
        cue_at = now;
        cue_second = false;
        cue_active = true;
        owns_auto.store(true);
        own_face = true;
        saved_emotion = sc.avatar().getEmotion();
        auto& motion = sc.motion();
        if (!motion_held) { saved_angles = motion.getCurrentAngles(); own_neck = true; }
        auto preset = [&](int yaw, int pitch, int speed, avatar::Emotion face,
                          uint8_t r, uint8_t g, uint8_t b) {
            sc.avatar().setEmotion(face);
            if (!motion_held) motion.moveWithSpeed(yaw, pitch, speed);
            if (!led_held) {
                own_led = true;
                sc.leftNeonLight().setColor(r,g,b);
                sc.rightNeonLight().setColor(r,g,b);
            }
        };
        switch (request) {
        case Cue::Nod: preset(0,90,180,avatar::Emotion::Neutral,40,50,70); break;
        case Cue::ShakeHead: preset(-120,120,180,avatar::Emotion::Doubt,50,50,60); break;
        case Cue::TiltHead: preset(140,120,150,avatar::Emotion::Doubt,75,65,35); break;
        case Cue::Happy: preset(-100,180,180,avatar::Emotion::Happy,110,30,65); break;
        case Cue::Surprise: preset(0,240,200,avatar::Emotion::Doubt,100,100,100); break;
        case Cue::Shy: preset(110,90,140,avatar::Emotion::Happy,90,35,55); break;
        default: break;
        }
    }
    void TickCue(StackChan& sc, uint32_t now) {
        const uint32_t elapsed = now - cue_at;
        auto& motion = sc.motion();
        if (elapsed >= 400 && !cue_second && !motion_held) {
            cue_second = true;
            if (cue == Cue::ShakeHead) motion.moveWithSpeed(120,120,180);
            if (cue == Cue::Happy) motion.moveWithSpeed(100,180,170);
            if (cue == Cue::Nod) motion.moveWithSpeed(0,170,160);
        }
        if (elapsed >= 850) {
            cue_active = false;
            Release(sc, true);
            phase = Phase::Boot;
            schedule.Reset();
        }
    }
};
Runtime r;
}

void Start() {
    // Start after the StackChanAvatarDisplay has attached its avatar.
    r = Runtime{};
    idle.store(false); interrupted.store(false);
    manual_motion.store(false); manual_led.store(false);
    queued_cue.store(-1); owns_auto.store(false);
    // Temporarily disable stage-1 runtime while investigating a post-update
    // audio/WebSocket regression. Preserve all code and existing firmware paths.
    // Re-enable only after an A/B test confirms playback stability.
    enabled.store(false);
}
void Stop() {
    enabled.store(false);
    r.ClearVisuals();
    r = Runtime{};
    owns_auto.store(false);
}
void OnStandby(bool ready) {
    idle.store(ready);
    if (!ready) interrupted.store(true);
}
void NotifyActivity() { interrupted.store(true); }
void CancelForExternalChange(StackChan& sc) {
    if (!enabled.load()) return;
    r.Release(sc, true);
    r.cue_active = false;
    r.phase = Phase::Boot;
    r.schedule.Reset();
    interrupted.store(true); // Reset idle deadlines even if the caller is a touch event.
}
void NotifyManualMotion() { manual_motion.store(true); interrupted.store(true); }
void NotifyManualLed() { manual_led.store(true); interrupted.store(true); }
bool IsAutonomousSceneActive() { return owns_auto.load(); }
bool RequestCue(const char* id) {
    if (!id || !enabled.load()) return false;
    for (int i = 0; i < static_cast<int>(Cue::Count); ++i)
        if (std::strcmp(id, cue_names[i]) == 0) { queued_cue.store(i); return true; }
    return false;
}
void Update(StackChan& sc) {
    if (!enabled.load() || !sc.hasAvatar()) return;
    const uint32_t now = GetHAL().millis();
    const bool event = interrupted.exchange(false);
    if (manual_motion.exchange(false)) r.motion_held = true;
    if (manual_led.exchange(false)) r.led_held = true;
    if (event && (r.own_face || r.cue_active)) {
        r.Release(sc, true); // explicit LED/neck ownership is respected by Release
        r.cue_active = false;
        r.phase = Phase::Boot;
        r.schedule.Reset();
    }
    int cue = queued_cue.exchange(-1);
    if (cue >= 0 && cue < static_cast<int>(Cue::Count)) {
        r.BeginCue(sc, static_cast<Cue>(cue), now);
    }
    if (r.cue_active) { r.TickCue(sc, now); return; }

    std::time_t epoch = std::time(nullptr);
    std::tm local{};
    const bool valid = epoch > 1700000000 && localtime_r(&epoch, &local) && local.tm_year >= 124;
    const auto minute = static_cast<uint16_t>(valid ? local.tm_hour * 60 + local.tm_min : 0);
    State state = r.schedule.Step({now,minute,valid,idle.load(),event,esp_random()});
    r.Begin(sc, state.phase, state.kind, now);
    // Limit scene redraws: the app update loop may run faster than animations
    // need, while each render mutates multiple LVGL objects.
    if (r.own_face && (r.last_draw_at == 0 || now - r.last_draw_at >= 100)) {
        r.Draw(state);
        r.last_draw_at = now;
    }
}

} // namespace stackchan::stage1
