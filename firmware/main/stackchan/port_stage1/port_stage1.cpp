// Stage-1 orchestration and persistent settings; no Gemini protocol changes.
#include "port_stage1.h"
#include "stage1_clock.h"
#include "rest_face.h"
#include "dream_face.h"
#include "dream_context.h"
#include <stackchan/stackchan.h>
#include <hal/hal.h>
#include <lvgl.h>
#include <esp_random.h>
#include <esp_log.h>
#include <nvs.h>
#include <esp_err.h>
#include <atomic>
#include <ctime>
#include <cstring>
#include <memory>
#include <cmath>

namespace stackchan::stage1 {
namespace {
std::atomic<bool> enabled{false};
std::atomic<bool> idle{false};
std::atomic<bool> interrupted{false};
std::atomic<bool> manual_motion{false};
std::atomic<bool> manual_led{false};
std::atomic<bool> owns_auto{false};
std::atomic<int> queued_cue{-1};
constexpr const char* LOG_TAG = "STAGE1";
constexpr const char* CONFIG_NS = "sc_stage1";

bool ValidConfig(const Config& c) {
    return c.wake_minute < 1440 && c.sleep_minute < 1440 &&
           c.wake_minute != c.sleep_minute && c.nap_chance <= 100 &&
           c.nap_chance % 5 == 0 &&
           c.theatre_min_seconds >= 10 && c.theatre_max_seconds <= 3600 &&
           c.theatre_min_seconds <= c.theatre_max_seconds &&
           c.theatre_min_seconds % 10 == 0 && c.theatre_max_seconds % 10 == 0 &&
           c.nap_idle_min_minutes >= 1 && c.nap_idle_max_minutes <= 1440 &&
           c.nap_idle_min_minutes <= c.nap_idle_max_minutes &&
           c.nap_sleep_min_minutes >= 1 && c.nap_sleep_max_minutes <= 1440 &&
           c.nap_sleep_min_minutes <= c.nap_sleep_max_minutes;
}

Config LoadConfig() {
    Config cfg{}; // Preserve the stage-1 defaults when no settings exist.
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return cfg;
    if (err != ESP_OK) {
        ESP_LOGW(LOG_TAG, "config NVS open failed: %s; using defaults", esp_err_to_name(err));
        return cfg;
    }
    uint8_t theatre = cfg.theatre, naps = cfg.naps;
    // Missing keys retain their defaults, allowing forward-compatible upgrades.
    nvs_get_u16(handle, "wake_min", &cfg.wake_minute);
    nvs_get_u16(handle, "sleep_min", &cfg.sleep_minute);
    nvs_get_u8(handle, "theatre", &theatre);
    nvs_get_u8(handle, "naps", &naps);
    nvs_get_u8(handle, "nap_chance", &cfg.nap_chance);
    nvs_get_u16(handle, "theatre_lo", &cfg.theatre_min_seconds);
    nvs_get_u16(handle, "theatre_hi", &cfg.theatre_max_seconds);
    nvs_get_u16(handle, "nap_idle_lo", &cfg.nap_idle_min_minutes);
    nvs_get_u16(handle, "nap_idle_hi", &cfg.nap_idle_max_minutes);
    nvs_get_u16(handle, "nap_sleep_lo", &cfg.nap_sleep_min_minutes);
    nvs_get_u16(handle, "nap_sleep_hi", &cfg.nap_sleep_max_minutes);
    nvs_close(handle);
    if (theatre > 1 || naps > 1) return Config{};
    cfg.theatre = theatre != 0;
    cfg.naps = naps != 0;
    if (!ValidConfig(cfg)) {
        ESP_LOGW(LOG_TAG, "invalid stage-1 NVS config; using defaults");
        return Config{};
    }
    return cfg;
}

bool SaveConfig(const Config& cfg) {
    if (!ValidConfig(cfg)) return false;
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(CONFIG_NS, NVS_READWRITE, &handle);
    if (opened != ESP_OK) {
        ESP_LOGE(LOG_TAG, "config NVS open failed: %s", esp_err_to_name(opened));
        return false;
    }
    esp_err_t err = nvs_set_u16(handle, "wake_min", cfg.wake_minute);
    if (err == ESP_OK) err = nvs_set_u16(handle, "sleep_min", cfg.sleep_minute);
    if (err == ESP_OK) err = nvs_set_u8(handle, "theatre", cfg.theatre ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(handle, "naps", cfg.naps ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_u8(handle, "nap_chance", cfg.nap_chance);
    if (err == ESP_OK) err = nvs_set_u16(handle, "theatre_lo", cfg.theatre_min_seconds);
    if (err == ESP_OK) err = nvs_set_u16(handle, "theatre_hi", cfg.theatre_max_seconds);
    if (err == ESP_OK) err = nvs_set_u16(handle, "nap_idle_lo", cfg.nap_idle_min_minutes);
    if (err == ESP_OK) err = nvs_set_u16(handle, "nap_idle_hi", cfg.nap_idle_max_minutes);
    if (err == ESP_OK) err = nvs_set_u16(handle, "nap_sleep_lo", cfg.nap_sleep_min_minutes);
    if (err == ESP_OK) err = nvs_set_u16(handle, "nap_sleep_hi", cfg.nap_sleep_max_minutes);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(LOG_TAG, "config NVS save failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

uint32_t last_status_log_at = 0;
bool status_logged = false;

enum class Cue : int { Nod, ShakeHead, TiltHead, Happy, Surprise, Shy, Count };
const char* const cue_names[] = {"nod", "shake_head", "tilt_head", "happy_move", "surprise_move", "shy_move"};

struct Runtime {
    struct FeaturePose {
        uitk::Vector2i position{0, 0};
        int weight = 0, size = 0, rotation = 0;
        static FeaturePose Capture(avatar::Feature& f) {
            return {f.getPosition(), f.getWeight(), f.getSize(), f.getRotation()};
        }
        void Restore(avatar::Feature& f) const {
            f.setPosition(position);
            f.setSize(size);
            f.setWeight(weight);
            f.setRotation(rotation);
        }
    };
    bool theatre_face_saved = false;
    FeaturePose saved_left, saved_right, saved_mouth;
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
    // Explicit gestures may temporarily move a manually positioned neck.
    bool cue_restore_neck = false;
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
        if (theatre_face_saved && restore) {
            saved_left.Restore(sc.avatar().leftEye());
            saved_right.Restore(sc.avatar().rightEye());
            saved_mouth.Restore(sc.avatar().mouth());
        }
        theatre_face_saved = false;
        if (own_led && !led_held && restore) {
            sc.leftNeonLight().setColor(0,0,0);
            sc.rightNeonLight().setColor(0,0,0);
        }
        // Only return an automatic posture when no external command took over.
        if (own_neck && restore && (!motion_held || cue_restore_neck))
            sc.motion().moveWithSpeed(saved_angles.x, saved_angles.y, 120);
        own_face = own_led = own_neck = false;
        cue_restore_neck = false;
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
        if (next == Phase::Theatre && !continuing) {
            saved_left = FeaturePose::Capture(sc.avatar().leftEye());
            saved_right = FeaturePose::Capture(sc.avatar().rightEye());
            saved_mouth = FeaturePose::Capture(sc.avatar().mouth());
            theatre_face_saved = true;
        }
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
    void ApplyTheatreFace(StackChan& sc, const State& s) {
        if (s.phase != Phase::Theatre || s.kind == Kind::Yawn) return;
        // Ported from S3-stackchan AvatarBehavior::UpdateIdlePose(). The
        // original firmware's static emotions alone do not animate a skit.
        const uint32_t elapsed = s.phase_elapsed_ms;
        int y = 0, mouth = 0, size = 0;
        int eye = (s.kind == Kind::Idea || s.kind == Kind::Shy) ? 72 :
                  (s.kind == Kind::Stretch ? 35 :
                  (s.kind == Kind::Hungry ? 70 : 75));
        switch (s.kind) {
        case Kind::Sneeze:
            if (elapsed >= 600 && elapsed < 1000) { eye = 25; mouth = 35; y = -15; }
            else if (elapsed >= 1000 && elapsed < 1250) { eye = 0; mouth = 100; y = 45; }
            break;
        case Kind::Hiccup: {
            const uint32_t beat = elapsed % 750;
            if (elapsed < 2250 && beat >= 250 && beat < 400) {
                y = -40; mouth = 65; eye = 100;
            }
            break;
        }
        case Kind::Hungry:
            mouth = elapsed % 1100 < 220 ? 25 : 0;
            break;
        case Kind::Stretch:
            if (elapsed < 1700) {
                const float pulse = std::sin(elapsed * 3.141592654f / 1700.0f);
                y = -static_cast<int>(pulse * 55);
                size = static_cast<int>(pulse * 18);
                mouth = static_cast<int>(pulse * 65);
            }
            break;
        case Kind::Idea:
            if (elapsed < 600) { eye = 55; mouth = 15; }
            else if (elapsed < 1100) { eye = 100; size = 15; mouth = 45; }
            break;
        case Kind::Shy:
            eye = elapsed % 700 < 180 ? 20 : 72;
            mouth = elapsed % 900 < 350 ? 30 : 0;
            break;
        default: break;
        }
        for (auto* feature : {&sc.avatar().leftEye(), &sc.avatar().rightEye()}) {
            feature->setPosition({0, y});
            feature->setWeight(eye);
            feature->setSize(size);
            feature->setRotation(0);
        }
        sc.avatar().mouth().setPosition({0, y});
        sc.avatar().mouth().setWeight(mouth);
        sc.avatar().mouth().setRotation(0);
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
        // Never discard an autonomous scene's original pose. The caller only
        // starts a cue when no scene is in control.
        Release(sc, true);
        cue = request;
        cue_at = now;
        cue_second = false;
        cue_active = true;
        owns_auto.store(true);
        // A live conversation owns the face/LED; gestures only move the neck.
        own_face = idle.load();
        saved_emotion = sc.avatar().getEmotion();
        auto& motion = sc.motion();
        saved_angles = motion.getCurrentAngles();
        own_neck = true;
        cue_restore_neck = true;
        auto preset = [&](int yaw, int pitch, int speed, avatar::Emotion face,
                          uint8_t r, uint8_t g, uint8_t b) {
            if (own_face) sc.avatar().setEmotion(face);
            motion.moveWithSpeed(yaw, pitch, speed);
            if (own_face && !led_held) {
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
        if (elapsed >= 400 && !cue_second) {
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
    const Config config = LoadConfig();
    if (!r.schedule.SetConfig(config)) {
        ESP_LOGE(LOG_TAG, "stage-1 config rejected; using defaults");
    }
    ESP_LOGI(LOG_TAG, "config wake=%u sleep=%u theatre=%d naps=%d chance=%u",
             config.wake_minute, config.sleep_minute, config.theatre,
             config.naps, config.nap_chance);
    idle.store(false); interrupted.store(false);
    manual_motion.store(false); manual_led.store(false);
    queued_cue.store(-1); owns_auto.store(false);
    // Re-enable stage-1 runtime for the controlled audio/WebSocket A/B test.
    // Keep the scene redraw rate limit in place.
    enabled.store(true);
    status_logged = false;
    last_status_log_at = 0;
    ESP_LOGI(LOG_TAG, "runtime started enabled=true");
}
void Stop() {
    enabled.store(false);
    r.ClearVisuals();
    r = Runtime{};
    queued_cue.store(-1);
    owns_auto.store(false);
}
// Caller holds the same LVGL lock used by Update(), so scene state is serialized.
Config GetConfig() { return r.schedule.GetConfig(); }
bool SetConfig(const Config& config) {
    if (!ValidConfig(config)) return false;
    if (!SaveConfig(config)) return false;
    if (!r.schedule.SetConfig(config)) return false;
    interrupted.store(true);
    ESP_LOGI(LOG_TAG, "stage-1 config saved and applied");
    return true;
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
    queued_cue.store(-1); // New explicit motion must not be followed by a stale gesture.
    r.phase = Phase::Boot;
    r.schedule.Reset();
    interrupted.store(true); // Reset idle deadlines even if the caller is a touch event.
}
void NotifyManualMotion() { manual_motion.store(true); interrupted.store(true); }
void NotifyManualLed() { manual_led.store(true); interrupted.store(true); }
bool IsAutonomousSceneActive() { return owns_auto.load(); }
bool RequestCue(const char* id) {
    if (!id || !enabled.load() || owns_auto.load()) return false;
    for (int i = 0; i < static_cast<int>(Cue::Count); ++i) {
        if (std::strcmp(id, cue_names[i]) != 0) continue;
        int empty = -1;
        // Refuse overlapping requests rather than replacing a queued action.
        return queued_cue.compare_exchange_strong(empty, i);
    }
    return false;
}
void Update(StackChan& sc) {
    if (!enabled.load() || !sc.hasAvatar()) return;
    const uint32_t now = GetHAL().millis();
    const bool event = interrupted.exchange(false);
    if (manual_motion.exchange(false)) {
        r.motion_held = true;
        // A newer manual command takes precedence over cue restoration.
        if (r.cue_active) r.cue_restore_neck = false;
    }
    if (manual_led.exchange(false)) r.led_held = true;
    if (event && (r.own_face || r.cue_active)) {
        r.Release(sc, true); // explicit LED/neck ownership is respected by Release
        r.cue_active = false;
        r.phase = Phase::Boot;
        r.schedule.Reset();
    }
    int cue = queued_cue.exchange(-1);
    if (cue >= 0 && cue < static_cast<int>(Cue::Count)) {
        // An automatic scene that began after queueing keeps ownership.
        if (!r.own_face && !r.cue_active)
            r.BeginCue(sc, static_cast<Cue>(cue), now);
        else ESP_LOGW(LOG_TAG, "cue skipped: autonomous scene is active");
    }
    if (r.cue_active) { r.TickCue(sc, now); return; }

    std::time_t epoch = std::time(nullptr);
    std::tm local{};
    const bool valid = epoch > 1700000000 && localtime_r(&epoch, &local) && local.tm_year >= 124;
    const auto minute = static_cast<uint16_t>(valid ? local.tm_hour * 60 + local.tm_min : 0);
    const bool standby = idle.load();
    State state = r.schedule.Step({now,minute,valid,standby,event,esp_random()});
    if (!status_logged || now - last_status_log_at >= 60000) {
        ESP_LOGI(LOG_TAG, "runtime phase=%d kind=%d standby=%d clock_valid=%d minute=%u auto=%d",
                 static_cast<int>(state.phase), static_cast<int>(state.kind),
                 standby, valid, static_cast<unsigned>(minute), r.own_face);
        status_logged = true;
        last_status_log_at = now;
    }
    if (state.phase != r.phase || state.kind != r.kind) {
        ESP_LOGI(LOG_TAG, "phase transition %d/%d -> %d/%d standby=%d",
                 static_cast<int>(r.phase), static_cast<int>(r.kind),
                 static_cast<int>(state.phase), static_cast<int>(state.kind), standby);
    }
    r.Begin(sc, state.phase, state.kind, now);
    // Limit scene redraws: the app update loop may run faster than animations
    // need, while each render mutates multiple LVGL objects.
    if (r.own_face && (r.last_draw_at == 0 || now - r.last_draw_at >= 100)) {
        r.Draw(state);
        r.ApplyTheatreFace(sc, state);
        r.last_draw_at = now;
    }
}

} // namespace stackchan::stage1
