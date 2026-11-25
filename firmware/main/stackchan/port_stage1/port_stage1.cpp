// Stage-1 orchestration and persistent settings; no Gemini protocol changes.
#include "port_stage1.h"
#include "stage1_clock.h"
#include "rest_face.h"
#include "dream_face.h"
#include "dream_context.h"
#include "thinking_face.h"
#include "listening_nod.h"
#include "avatar_config.h"
#include "voice_settings.h"
#include "voice_client.h"
#include <stackchan/speech_mouth.h>
#include "quiet_mode.h"
#include "quiet_mark.h"
#include "environment.h"
#include "weather_face.h"
#include "weather_changes.h"
#include "speech_effects.h"
#include <stackchan/avatar/skins/default/default.h>
#include <stackchan/avatar/face_snapshot.h>
#include <application.h>
#include <stackchan/stackchan.h>
#include <hal/hal.h>
#include <lvgl.h>
#include <esp_lvgl_port.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <nvs.h>
#include <esp_err.h>
#include <atomic>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <memory>
#include <cmath>

extern "C" void stackchan_nvs_commit_observer(void);

namespace stackchan::stage1 {
namespace {
std::atomic<bool> enabled{false};
// Read by the audio/WebSocket task without touching Stage1's LVGL-owned clock.
std::atomic<uint16_t> registered_wake_minute{6 * 60};
std::atomic<bool> idle{false};
std::atomic<bool> interrupted{false};
std::atomic<bool> manual_motion{false};
std::atomic<bool> manual_led{false};
// Keep manual/explicit requests newer than the current speech turn visible
// even after Update consumes the original one-shot interruption flags.
std::atomic<uint32_t> speech_motion_epoch{0};
std::atomic<uint32_t> speech_led_epoch{0};
std::atomic<bool> owns_auto{false};
std::atomic<int> queued_cue{-1};
std::atomic<bool> manual_sleep_requested{false};
std::atomic<uint32_t> manual_sleep_requested_at{0};
std::atomic<int> perception_yaw_delta{0};
std::atomic<int> perception_pitch_delta{0};
std::atomic<uint32_t> perception_requested_at{0};
std::atomic<bool> perception_look_pending{false};
std::atomic<bool> perception_look_direct{false};
std::atomic<bool> speech_tracking_paused{false};
// Worker publishes requests/results; only the existing Stage1 update owns servos.
std::atomic<uint32_t> search_start_epoch{0};
std::atomic<int> search_start{0}; // signed DOA in tenths of degrees, 1001 = zero
std::atomic<bool> search_cancel{false};
std::atomic<bool> search_active{false};
std::atomic<bool> wake_pitch_pending{false};
struct PersonSearch {
    enum Phase { First = 1, Reverse, Return };
    Phase phase = First;
    int direction = 1, origin = 0, target = 0;
    bool running = false, commanded = false;
    uint32_t revision = 0, epoch = 0;
    void StopOwned(StackChan& sc) {
        if (commanded && sc.motion().yawServo().getCommandRevision() == revision)
            sc.motion().moveYawWithSpeed(sc.motion().getCurrentYawAngle(), 1000);
        commanded = false;
    }
    void End(StackChan& sc, bool stop) {
        if (stop) StopOwned(sc);
        *this = PersonSearch{};
        search_active.store(false);
    }
    void Pause(StackChan& sc) {
        if (!running) return;
        StopOwned(sc);
    }
};
PersonSearch person_search;
// LVGL is owned by Update(). Head pad/voice tasks only set these flags.
std::atomic<uint8_t> wake_intents{0}; // touch=1, wake word=2
std::atomic<bool> sleep_wake_eligible{false};
std::atomic<bool> wake_sequence_active{false};
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
    if (err == ESP_OK) stackchan_nvs_commit_observer();
    else ESP_LOGE(LOG_TAG, "config NVS save failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

// S3 timed-nap wake: the last actually shown dream is remembered only until
// its optional, short post-wake remark. No extra worker or permanent task.
struct DreamRemark {
    avatar_dream::Motif motif = avatar_dream::Motif::None;
    bool timer_requested = false;
    uint32_t selected_at = 0;
    uint32_t due_at = 0;
};
DreamRemark dream_remark{};
weather::Changes weather_changes;
std::atomic<bool> dream_remark_cancel_requested{false};
void CancelDreamRemark() {
    dream_remark = {};
    dream_remark_cancel_requested.store(false);
}
bool VoicePending() { return voice::Pending(); }
// The scene chooses the utterance and dream context; voice_client owns
// queueing, audio lifecycle, completion/retry timing, and error rearming.
void VoiceTick(Phase old_phase, const State& state, uint32_t now) {
    const auto* config = voice::Current();
    const bool entered = old_phase != state.phase;
    if (entered) {
        if (old_phase == Phase::NapSleep) voice::LeaveNap();
        voice::SceneChanged(state.phase, now);
    }
    if (!config) return;
    const bool quiet = quiet_mode::Active();
    if (quiet) {
        if (voice::SleepActive()) avatar_dream::EndTalk();
        voice::Quiet();
    }
    if (entered) {
        switch (state.phase) {
        case Phase::NightMessage:
            if (config->bedtime[0]) voice::QueueScene(voice::Kind::Bedtime);
            break;
        case Phase::MorningWake:
            if (config->morning[0]) voice::QueueScene(voice::Kind::Morning);
            break;
        case Phase::NapWake:
        case Phase::NightWake:
            if (state.wake_reason == WakeReason::Touch && config->touch[0])
                voice::QueueScene(voice::Kind::Touch);
            else if (!quiet && state.wake_reason == WakeReason::Timer && config->timer[0])
                voice::QueueScene(voice::Kind::Timer);
            // Wake-word speech belongs to Xiaozhi; never queue a duplicate.
            break;
        case Phase::NapSleep:
            if (!quiet) voice::EnterNap(*config, now);
            break;
        default: break;
        }
    }
    if (voice::TickScene(*config, state.phase, now) &&
        dream_remark.motif != avatar_dream::Motif::None)
        dream_remark.timer_requested = true;
    if (state.phase != Phase::NapSleep || quiet) return;

    // During a nap, completion or send failure rearms the same voice kind.
    voice::ResumeNap(*config, now);
    if (voice::IsBusy()) {
        voice::DiscardDueSleep(*config, now);
        return;
    }
    if (voice::FinishSleepIfIdle(*config, now) == voice::Kind::SleepTalk)
        avatar_dream::EndTalk();

    // Dream utterances take priority and are tied to the currently visible motif.
    if (voice::Due(voice::Kind::SleepTalk, now)) {
        const auto motif = avatar_dream::BeginTalk();
        char prompt[513];
        std::snprintf(prompt, sizeof(prompt), "%s", config->sleep_talk_prompt);
        if (const char* name = avatar_dream::Name(motif)) {
            const size_t used = std::strlen(prompt);
            std::snprintf(prompt + used, sizeof(prompt) - used,
                          "\n見ている夢は「%s」です。この夢に合った寝言にしてください。", name);
        }
        if (voice::RequestSleep(voice::Kind::SleepTalk, *config, prompt, now)) {
            ESP_LOGI(LOG_TAG, "[SPEECH] kind=sleep_talk phase=requested dream=%s",
                     avatar_dream::Name(motif) ? avatar_dream::Name(motif) : "なし");
            // Sleep talk won this tick; any simultaneous snore is discarded.
            voice::DiscardDueSleep(*config, now);
            return;
        }
        avatar_dream::EndTalk();
    }
    if (voice::Due(voice::Kind::Snore, now))
        voice::RequestSleep(voice::Kind::Snore, *config, config->snore, now);
}
uint32_t last_status_log_at = 0;
bool status_logged = false;

enum class Cue : int { Nod, ShakeHead, TiltHead, Happy, Surprise, Shy, Count };
const char* const cue_names[] = {"nod", "shake_head", "tilt_head", "happy_move", "surprise_move", "shy_move"};

struct Runtime {
    bool scene_face_saved = false;
    avatar::FaceSnapshot saved_face;
    // The S3 conversation face owns only visuals; stage-1 still owns
    // scheduling, servo actions, sleep and the existing chat/audio flow.
    ConversationPose conversation_pose = ConversationPose::None;
    bool conversation_saved = false;
    avatar::FaceSnapshot conversation_face;
    uint32_t conversation_at = 0, listening_nod_at = 0;
    bool listening_nod = false;
    std::unique_ptr<AvatarThinkingFace> thinking_face;
    Clock schedule;
    Phase phase = Phase::Boot;
    Kind kind = Kind::None;
    uint32_t phase_at = 0;
    uint32_t last_draw_at = 0;
    bool motion_held = false;
    uitk::Vector2i sleep_home_origin{0, 0};
    bool sleep_yawn_return_started = false;
    // S3 night sleep keeps its face visible for 3 s, then turns off only the
    // backlight. Never persist this transient brightness to NVS.
    bool night_light_armed = false, night_light_off = false;
    uint32_t night_light_due = 0;
    uint8_t night_light_original = 0;
    bool led_held = false;
    bool own_face = false;
    bool own_led = false;
    bool own_neck = false;
    bool cue_active = false;
    Cue cue = Cue::Nod;
    uint32_t cue_at = 0;
    uint8_t cue_step = 0;
    uint8_t wake_step = 0;
    uint32_t last_sleep_elapsed = 0;
    bool waking_from_sleep = false;
    bool own_speech = false;
    WakeReason last_wake_reason = WakeReason::None;
    // Restore the visibility of the existing avatar features after the
    // S3 Startled overlay, without modifying their expression or geometry.
    bool startled_face_active = false;
    bool startled_left_visible = true;
    bool startled_right_visible = true;
    bool startled_mouth_visible = true;
    // Theatre/actions keep their final position; sleep retains prior behaviour.
    bool restore_neck_on_release = false;
    uint32_t theatre_motion_protect_until = 0;
    uitk::Vector2i saved_angles{0, 0};
    std::unique_ptr<AvatarRestFace> rest;
    std::unique_ptr<AvatarDreamFace> dream;
    std::unique_ptr<AvatarQuietMark> quiet_mark;
    std::unique_ptr<AvatarWeatherFace> weather_face;
    uint32_t environment_poll_at = 0;

    // All LVGL work stays in Update's existing display lock. The source
    // mouth is the original DefaultAvatar or the S3-style rest face.
    void SyncQuietMark(StackChan& sc, bool visible) {
        if (!visible) { quiet_mark.reset(); return; }
        lv_obj_t* mouth = rest ? rest->MouthObject() :
            static_cast<avatar::DefaultMouth&>(sc.avatar().mouth()).getObject();
        if (!mouth) { quiet_mark.reset(); return; }
        if (!quiet_mark)
            quiet_mark = std::make_unique<AvatarQuietMark>(lv_screen_active());
        quiet_mark->Render(mouth);
    }

    // The weather badge does not overlap the speech bubble and stays visible.
    // Only the face-level temperature decoration (scarf or heat drops) yields
    // while the actual bubble is visible, including subtitle review after
    // audio playback ends.
    void SyncWeather(StackChan& sc, uint32_t now, bool suppress_comfort) {
        auto appearance = environment::ReadAppearance();
        const auto& bubble = sc.avatar().getKeyElements().speechBubble;
        if (!appearance.visible) {
            weather_face.reset();
            return;
        }
        const bool bubble_overlaps_cold = bubble && bubble->getVisible() &&
                                          appearance.comfort == weather::Comfort::Cold;
        if (suppress_comfort || bubble_overlaps_cold)
            appearance.comfort = weather::Comfort::None;
        if (!weather_face)
            weather_face = std::make_unique<AvatarWeatherFace>(lv_screen_active());
        weather_face->Render(appearance, now);
    }

    void RestoreNightBacklight() {
        if (night_light_off) {
            GetHAL().setBackLightBrightness(night_light_original, false);
            ESP_LOGI(LOG_TAG, "night sleep backlight restored: %u", night_light_original);
        }
        night_light_armed = night_light_off = false;
        night_light_due = 0;
    }
    void TickNightBacklight(uint32_t now) {
        if (!night_light_armed || night_light_off || phase != Phase::NightSleep ||
            static_cast<int32_t>(now - night_light_due) < 0) return;
        // Ensure the sleeping face is visible for the full grace period.
        GetHAL().setBackLightBrightness(0, false);
        night_light_off = true;
        ESP_LOGI(LOG_TAG, "night sleep backlight off (3s after sleep face)");
    }
    static bool Sleeping(Phase p) {
        return p == Phase::NapSleep || p == Phase::NightSleep;
    }
    static bool SleepYawn(Phase p) {
        return p == Phase::NapYawn || p == Phase::NightYawn;
    }
    static bool SleepPreparing(Phase p) {
        return p == Phase::NapDrowsy || p == Phase::NightDrowsy ||
               p == Phase::NightMessage || SleepYawn(p);
    }
    static uitk::Vector2i Home(motion::Motion& motion) {
        const auto yaw = motion.yawServo().getAngleLimit();
        const auto pitch = motion.pitchServo().getAngleLimit();
        return {uitk::clamp(0, yaw.x, yaw.y), uitk::clamp(0, pitch.x, pitch.y)};
    }
    void MoveSleepHome(motion::Motion& motion, int numerator, int denominator,
                       uint32_t duration, uint32_t now) {
        const auto home = Home(motion);
        const int yaw = sleep_home_origin.x + (home.x - sleep_home_origin.x) * numerator / denominator;
        const int pitch = sleep_home_origin.y + (home.y - sleep_home_origin.y) * numerator / denominator;
        motion.moveWithDuration(yaw, pitch, duration, now);
        ESP_LOGI(LOG_TAG, "sleep prepare: phase=%d target=(%d,%d) duration=%u",
                 static_cast<int>(phase), yaw, pitch, static_cast<unsigned>(duration));
    }
    static bool Auto(Phase p) {
        return p == Phase::Theatre || p == Phase::NapDrowsy || p == Phase::NapYawn ||
               p == Phase::NapSleep || p == Phase::NapWake || p == Phase::NightDrowsy ||
               p == Phase::NightMessage || p == Phase::NightYawn || p == Phase::NightSleep ||
               p == Phase::NightWake || p == Phase::MorningWake;
    }
    void EndConversation(StackChan& sc) {
        if (conversation_pose == ConversationPose::None) return;
        thinking_face.reset();
        if (conversation_saved) conversation_face.Restore(sc.avatar());
        conversation_saved = false;
        listening_nod = false;
        conversation_pose = ConversationPose::None;
    }
    void BeginConversation(StackChan& sc, ConversationPose pose, uint32_t now) {
        if (pose == ConversationPose::None) { EndConversation(sc); return; }
        if (conversation_pose == pose) return;
        if (!conversation_saved) {
            if (phase == Phase::NightSleep || phase == Phase::NightTemporary)
                ESP_LOGW(LOG_TAG, "[SLEEP_DIAG] reset source=conversation_pose phase=%d pose=%d",
                         static_cast<int>(phase), static_cast<int>(pose));
            RestoreNightBacklight();
            Release(sc, true);
            sc.prepareFaceTakeover();
            cue_active = false;
            queued_cue.store(-1);
            phase = Phase::Boot;
            schedule.Reset();
            auto& av = sc.avatar();
            conversation_face = avatar::FaceSnapshot::Capture(av);
            conversation_saved = true;
            // S3 conversation poses replace previous emotions/decorations,
            // not just their eye and mouth geometry. Restore on exit.
            av.setEmotion(avatar::Emotion::Neutral);
        }
        if (pose == ConversationPose::Listening) {
            thinking_face.reset();
            listening_nod = false;
        }
        conversation_pose = pose;
        conversation_at = now;
        if (pose == ConversationPose::Thinking) {
            listening_nod = true;
            listening_nod_at = now;
        }
    }
    void DrawConversation(StackChan& sc, uint32_t now) {
        auto& av = sc.avatar();
        const bool nodding = conversation_pose == ConversationPose::Thinking &&
                             listening_nod &&
                             now - listening_nod_at < avatar_nod::DURATION_MS;
        if (conversation_pose == ConversationPose::Thinking && !nodding) {
            listening_nod = false;
            // Reuse S3's dedicated eyes, mouth and three-dot thinking cloud.
            for (auto* feature : {&av.leftEye(), &av.rightEye(), &av.mouth()})
                feature->setVisible(false);
            if (!thinking_face)
                thinking_face = std::make_unique<AvatarThinkingFace>(lv_screen_active());
            thinking_face->Render(false, 0, now - conversation_at);
            return;
        }
        thinking_face.reset();
        for (auto* feature : {&av.leftEye(), &av.rightEye(), &av.mouth()})
            feature->setVisible(true);
        const bool blink = (now - conversation_at) % avatar_config::CONVERSATION_BLINK_PERIOD_MS >=
            avatar_config::CONVERSATION_BLINK_PERIOD_MS - avatar_config::CONVERSATION_BLINK_MS;
        const int offset = nodding ? avatar_nod::Offset(now - listening_nod_at) : -5;
        for (auto* eye : {&av.leftEye(), &av.rightEye()}) {
            eye->setPosition({0, offset});
            eye->setSize(nodding ? 0 : 6);
            eye->setWeight(blink && !nodding ? 0 : 100);
        }
        av.mouth().setPosition({0, nodding ? offset : 0});
        av.mouth().setWeight(0);
    }
    void ClearVisuals() {
        last_draw_at = 0;
        dream.reset();
        rest.reset();
        avatar_dream::End();
    }
    void Release(StackChan& sc, bool restore) {
        // Releasing a sleep scene must restore other LED owners as well.
        GetHAL().setSleepRgbBlackout(false);
        // Also covers explicit activity/gesture interruptions which do not
        // pass through Begin(), so night sleep never leaves the LCD dark.
        RestoreNightBacklight();
        if (own_speech) { sc.avatar().clearSpeech(); own_speech = false; }
        ClearVisuals();
        wake_sequence_active.store(false);
        if (startled_face_active) {
            sc.avatar().leftEye().setVisible(startled_left_visible);
            sc.avatar().rightEye().setVisible(startled_right_visible);
            sc.avatar().mouth().setVisible(startled_mouth_visible);
            startled_face_active = false;
        }
        if (scene_face_saved && restore) saved_face.Restore(sc.avatar());
        scene_face_saved = false;
        if (own_led && !led_held && restore) {
            sc.leftNeonLight().setColor(0,0,0);
            sc.rightNeonLight().setColor(0,0,0);
        }
        // Theatre and explicit gestures finish at their final servo position.
        // Restore only the existing sleep/wake sequence when not overridden.
        if (SleepPreparing(phase) || Sleeping(phase)) sc.motion().cancelTimedMove();
        if (own_neck && restore && restore_neck_on_release && !motion_held)
            sc.motion().moveWithSpeed(saved_angles.x, saved_angles.y, 120);
        own_face = own_led = own_neck = false;
        restore_neck_on_release = false;
        sleep_yawn_return_started = false;
        motion_held = false; // Manual override belongs to a scene, not permanently.
        owns_auto.store(false);
    }
    void Begin(StackChan& sc, Phase next, Kind next_kind, uint32_t now) {
        if (phase == next && kind == next_kind) return;
        if (next != Phase::NightSleep) RestoreNightBacklight();
        if ((SleepPreparing(phase) || Sleeping(phase)) &&
            !SleepPreparing(next) && !Sleeping(next)) {
            sc.motion().cancelTimedMove();
            sleep_yawn_return_started = false;
        }
        // Preserve the pre-automation face/pose across a multi-step night/nap scene.
        const bool continuing = own_face && Auto(next);
        if (!continuing && Auto(next)) sc.prepareFaceTakeover();
        if (own_speech) { sc.avatar().clearSpeech(); own_speech = false; }
        if (continuing) ClearVisuals();
        else Release(sc, true);
        if (next == Phase::NapWake || next == Phase::NightWake) {
            waking_from_sleep = Sleeping(phase);
            last_sleep_elapsed = now - phase_at;
        }
        if (!Sleeping(next)) {
            // A continuous sleep-to-wake sequence does not pass Release().
            if (Sleeping(phase)) GetHAL().setSleepRgbBlackout(false);
        }
        phase = next;
        kind = next_kind;
        phase_at = now;
        if (next == Phase::NightSleep) {
            night_light_original = GetHAL().getBackLightBrightness();
            night_light_armed = true;
            night_light_off = false;
            night_light_due = now + 3000; // S3 NIGHT_SLEEP_BACKLIGHT_DELAY_MS
        }
        if (!Auto(next)) return;
        // Preserve the complete baseline once across a multi-phase scene.
        if (!continuing) {
            saved_face = avatar::FaceSnapshot::Capture(sc.avatar());
            scene_face_saved = true;
        }
        own_face = true;
        owns_auto.store(true);
        restore_neck_on_release = next != Phase::Theatre;
        if (next == Phase::Theatre) theatre_motion_protect_until = now + 3000;
        if (next == Phase::NapWake || next == Phase::NightWake ||
            next == Phase::MorningWake) {
            wake_step = 0;
            wake_sequence_active.store(true);
        }
        auto& av = sc.avatar();
        auto& motion = sc.motion();
        // Autonomous motion never overrides a retained explicit neck position.
        auto move = [&](int yaw, int pitch, int speed) {
            if (motion_held) return;
            if (!own_neck) { saved_angles = motion.getCurrentAngles(); own_neck = true; }
            motion.moveWithSpeed(yaw, pitch, speed);
        };
        auto move_relative = [&](int yaw_delta, int pitch_delta, int speed) {
            if (motion_held) return;
            const auto origin = motion.getCurrentAngles();
            if (!own_neck) { saved_angles = origin; own_neck = true; }
            motion.moveRelativeFrom(origin, yaw_delta, pitch_delta, speed);
        };
        auto say = [&](const char* words) {
            // Existing StackChan has a visual speech bubble. Keep its
            // transport untouched; this is not an extra TTS worker.
            if (!words || !*words) return;
            av.setSpeech(words);
            own_speech = true;
        };
        auto color = [&](uint8_t r,uint8_t g,uint8_t b) {
            if (led_held) return;
            own_led = true;
            sc.leftNeonLight().setColor(r,g,b);
            sc.rightNeonLight().setColor(r,g,b);
        };
        if (next == Phase::Theatre) {
            switch(next_kind) {
            case Kind::Sneeze: av.setEmotion(avatar::Emotion::Doubt); move_relative(0,-90,250); color(55,55,75); break;
            case Kind::Hiccup: av.setEmotion(avatar::Emotion::Doubt); move_relative(0,30,200); color(60,50,30); break;
            case Kind::Hungry: av.setEmotion(avatar::Emotion::Sad); move_relative(0,-70,120); color(45,25,10); break;
            case Kind::Stretch: av.setEmotion(avatar::Emotion::Sleepy); move_relative(0,90,120); color(70,60,40); break;
            case Kind::Idea: av.setEmotion(avatar::Emotion::Happy); move_relative(50,70,160); color(100,90,20); break;
            case Kind::Shy: av.setEmotion(avatar::Emotion::Happy); move_relative(110,30,130); color(100,30,60); break;
            case Kind::Yawn: av.setEmotion(avatar::Emotion::Sleepy); color(45,45,65); break;
            default: break;
            }
        } else if (next == Phase::NapDrowsy || next == Phase::NightDrowsy) {
            av.setEmotion(avatar::Emotion::Sleepy);
            if (!motion_held) {
                sleep_home_origin = motion.getCurrentAngles();
                if (!own_neck) { saved_angles = sleep_home_origin; own_neck = true; }
                sleep_yawn_return_started = false;
                // Identical movement for nap and night, scaled to this scene's time.
                MoveSleepHome(motion, 2, 3, schedule.DrowsyDurationMs() - 300, now);
            }
            color(28,18,20);
            ESP_LOGI(LOG_TAG, "sleep action: drowsy");
        } else if (next == Phase::NightMessage) {
            av.setEmotion(avatar::Emotion::Sleepy);
            if (!motion_held) MoveSleepHome(motion, 5, 6, kNightMessageMinMs - 300, now);
            say(voice::Current() ? voice::Current()->bedtime : "そろそろ寝るね。おやすみなさい。");
            color(28,18,20);
            ESP_LOGI(LOG_TAG, "S3 bedtime message");
        } else if (next == Phase::NapYawn || next == Phase::NightYawn) {
            av.setEmotion(avatar::Emotion::Sleepy);
            if (!motion_held) {
                if (!own_neck) { saved_angles = motion.getCurrentAngles(); own_neck = true; }
                const auto home = Home(motion);
                const auto limit = motion.pitchServo().getAngleLimit();
                sleep_yawn_return_started = false;
                motion.yawServo().moveWithDuration(home.x, kSleepYawnMs - 500, now);
                // Keep the downward yawn gesture on both sleep paths.
                const int dip = uitk::clamp(motion.getCurrentPitchAngle() - 85, limit.x, limit.y);
                motion.pitchServo().moveWithDuration(dip, 1000, now);
            }
            ESP_LOGI(LOG_TAG, "sleep action: yawn");
        } else if (Sleeping(next)) {
            av.setEmotion(avatar::Emotion::Sleepy);
            if (!own_neck) { saved_angles = motion.getCurrentAngles(); own_neck = true; }
            // All sleep entry paths have completed the preparation/yawn.
            // Never launch a fresh fast home command after the sleeping face.
            motion.cancelTimedMove();
            sleep_yawn_return_started = false;
            // S3 sleep state: all 12 neck LEDs must be dark. Explicit sleep
            // overrides any colour left by an earlier autonomous animation.
            own_led = true;
            led_held = false;
            sc.leftNeonLight().setColor(0, 0, 0);
            sc.rightNeonLight().setColor(0, 0, 0);
            GetHAL().setSleepRgbBlackout(true);
            ESP_LOGI(LOG_TAG, "sleep action: settled; LEDs off");
            if (next == Phase::NapSleep) avatar_dream::Begin();
        } else if (next == Phase::NapWake || next == Phase::NightWake) {
            // S3 DayNap wake: briefly show the popping sleeping bubble, then
            // the crossed-eye Startled face and source-specific greeting.
            last_wake_reason = schedule.CurrentWakeReason();
            av.setEmotion(avatar::Emotion::Neutral);
            startled_left_visible = av.leftEye().getVisible();
            startled_right_visible = av.rightEye().getVisible();
            startled_mouth_visible = av.mouth().getVisible();
            av.leftEye().setVisible(false);
            av.rightEye().setVisible(false);
            av.mouth().setVisible(false);
            startled_face_active = true;
            // Touch wake subtitles now come only from the 12800 WebSocket,
            // paced with the actual audio. Do not show the stored full text
            // locally first; word/timer wake retain their original captions.
            if (last_wake_reason == WakeReason::Word)
                say(voice::Current() ? voice::Current()->wake_word : "あ、呼んだ？ 聞いてるよ。");
            else if (last_wake_reason != WakeReason::Touch)
                say(voice::Current() ? voice::Current()->timer : "よく寝た。そろそろ起きるね。");
            if (!motion_held) {
                if (!own_neck) { saved_angles = motion.getCurrentAngles(); own_neck = true; }
                motion.moveRelativeFrom(motion.getCurrentAngles(), 0, 150, 330);
            }
            color(90,80,65);
            ESP_LOGI(LOG_TAG, "S3 wake: reason=%d", static_cast<int>(last_wake_reason));
        } else if (next == Phase::MorningWake) {
            // S3 scheduled morning is NOT a startled wake. It starts sleepy,
            // then returns to the normal face with the morning greeting.
            av.setEmotion(avatar::Emotion::Sleepy);
            say(voice::Current() ? voice::Current()->morning : "おはよう。今日もよろしくね。");
            if (!motion_held) {
                if (!own_neck) { saved_angles = motion.getCurrentAngles(); own_neck = true; }
                motion.moveRelativeFrom(motion.getCurrentAngles(), 0, 90, 170);
            }
            color(65,48,28);
            ESP_LOGI(LOG_TAG, "S3 morning: sleepy face and greeting");
        }
    }
    void TickSleepYawn(StackChan& sc, uint32_t elapsed, uint32_t now) {
        if (motion_held || sleep_yawn_return_started || elapsed < 1200) return;
        sleep_yawn_return_started = true;
        auto& motion = sc.motion();
        const auto home = Home(motion);
        // Leave 500ms for physical settling, not an additional sleep delay.
        const uint32_t finish = kSleepYawnMs - 500;
        motion.pitchServo().moveWithDuration(home.y, elapsed < finish ? finish - elapsed : 500, now);
        ESP_LOGI(LOG_TAG, "sleep prepare: final yawn target=(%d,%d)", home.x, home.y);
    }
    bool SleepHomeReady(StackChan& sc, uint32_t now) {
        // Poll movement only at the end of the yawn. Do not block the
        // scheduler while a servo is settling or if it has stalled.
        if (!SleepYawn(phase) || motion_held) return true;
        if (now - phase_at < kSleepYawnMs || !sleep_yawn_return_started) return false;
        return !sc.motion().isMoving();
    }
    void TickWake(StackChan& sc, uint32_t elapsed) {
        if (motion_held) return;
        // Staged wake movement, independent of render rate. The scheduler
        // releases ownership at 2800ms and retains the existing final restore.
        if (elapsed >= 1100 && wake_step == 0) {
            wake_step = 1;
            if (phase == Phase::MorningWake)
                sc.avatar().setEmotion(avatar::Emotion::Neutral);
            sc.motion().moveRelativeFrom(saved_angles, 0, 35, 190);
        }
        if (elapsed >= 2050 && wake_step == 1) {
            wake_step = 2;
            sc.motion().moveRelativeFrom(saved_angles, 0, 0, 180);
            // The final wake step already requests the saved position.
            // Avoid commanding that same pose again on scene release.
            restore_neck_on_release = false;
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
    void Draw(StackChan& sc, const State& s) {
        const bool asleep = Sleeping(s.phase);
        const bool startled_wake = s.phase == Phase::NapWake || s.phase == Phase::NightWake;
        const bool yawn = s.phase == Phase::NapYawn || s.phase == Phase::NightYawn ||
                          (s.phase == Phase::Theatre && s.kind == Kind::Yawn);
        if (asleep || yawn || startled_wake) {
            if (!rest) {
                // The caption belongs to DefaultAvatar's panel. Keep the
                // full-screen sleep mask in that same parent so the real
                // speech bubble can be raised above it during snores/talk.
                auto& avatar = static_cast<avatar::DefaultAvatar&>(sc.avatar());
                rest = std::make_unique<AvatarRestFace>(avatar.getPanel()->get());
            }
            if (startled_wake && waking_from_sleep && s.phase_elapsed_ms < 240)
                rest->RenderWakePop(last_sleep_elapsed, s.phase_elapsed_ms);
            else {
                const uint8_t sleep_sound =
                    s.phase == Phase::NapSleep ? voice::SleepSoundPlaying() : 0;
                // Snoring and sleep-talk reuse the same speaker PCM level
                // as ordinary speech rather than an unrelated sine wave.
                const uint8_t mouth = sleep_sound ? speech_mouth::Opening(GetHAL().millis()) : 0;
                rest->Render(startled_wake ? AvatarRestPose::Startled :
                             asleep ? AvatarRestPose::Sleeping : AvatarRestPose::Yawning,
                             sleep_sound != 0, mouth, s.phase_elapsed_ms,
                             sleep_sound != 0);
            }
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
        if (idle.load()) sc.prepareFaceTakeover();
        cue = request;
        cue_at = now;
        cue_step = 0;
        cue_active = true;
        owns_auto.store(true);
        // A live conversation owns the face/LED; gestures only move the neck.
        own_face = idle.load();
        if (own_face) {
            saved_face = avatar::FaceSnapshot::Capture(sc.avatar());
            scene_face_saved = true;
        }
        auto& motion = sc.motion();
        saved_angles = motion.getCurrentAngles();
        own_neck = true;
        restore_neck_on_release = false;
        ESP_LOGI(LOG_TAG, "servo action begin: %s face=%d", cue_names[static_cast<int>(request)], own_face);
        auto preset = [&](int delta_yaw, int delta_pitch, int speed, avatar::Emotion face,
                          uint8_t r, uint8_t g, uint8_t b) {
            if (own_face) sc.avatar().setEmotion(face);
            motion.moveRelativeFrom(saved_angles, delta_yaw, delta_pitch, speed);
            if (own_face && !led_held) {
                own_led = true;
                sc.leftNeonLight().setColor(r,g,b);
                sc.rightNeonLight().setColor(r,g,b);
            }
        };
        switch (request) {
        // Both actions return to the same position they started at.
        case Cue::Nod: preset(kTestNod.yaw,kTestNod.pitch,kTestNod.speed,avatar::Emotion::Neutral,40,50,70); break;
        case Cue::ShakeHead: preset(kTestShakeLeft.yaw,kTestShakeLeft.pitch,kTestShakeLeft.speed,avatar::Emotion::Doubt,50,50,60); break;
        case Cue::TiltHead: preset(kTestTilt.yaw,kTestTilt.pitch,kTestTilt.speed,avatar::Emotion::Doubt,75,65,35); break;
        case Cue::Happy: preset(kTestHappy.yaw,kTestHappy.pitch,kTestHappy.speed,avatar::Emotion::Happy,110,30,65); break;
        case Cue::Surprise:
            // Fast upward startle; clamp in MoveRelative protects the neck.
            preset(kTestSurprise.yaw,kTestSurprise.pitch,kTestSurprise.speed,avatar::Emotion::Neutral,100,100,100);
            if (own_face) {
                // S3 AvatarRestPose::Startled: crossed eyes, sweat and
                // dedicated mouth. Hide the normal face underneath it.
                startled_left_visible = sc.avatar().leftEye().getVisible();
                startled_right_visible = sc.avatar().rightEye().getVisible();
                startled_mouth_visible = sc.avatar().mouth().getVisible();
                sc.avatar().leftEye().setVisible(false);
                sc.avatar().rightEye().setVisible(false);
                sc.avatar().mouth().setVisible(false);
                startled_face_active = true;
                rest = std::make_unique<AvatarRestFace>(lv_screen_active());
                rest->Render(AvatarRestPose::Startled, false, 0, 0);
            }
            break;
        case Cue::Shy: preset(kTestShy.yaw,kTestShy.pitch,kTestShy.speed,avatar::Emotion::Happy,90,35,55); break;
        default: break;
        }
    }
    void TickCue(StackChan& sc, uint32_t now) {
        const uint32_t elapsed = now - cue_at;
        auto& motion = sc.motion();
        if (cue == Cue::ShakeHead) {
            // Two complete left/right sweeps, then the exact starting target.
            if (cue_step < 4 && elapsed >= (cue_step + 1) * kTestShakeStepMs) {
                ++cue_step;
                const auto target = TestShakeTarget(cue_step);
                motion.moveRelativeFrom(saved_angles, target.yaw, target.pitch, target.speed);
            }
        } else if (cue == Cue::Nod) {
            // A deeper nod than the previous -80 unit movement.
            if (cue_step == 0 && elapsed >= 650) {
                motion.moveRelativeFrom(saved_angles, 0, 0, 270);
                cue_step = 1;
            }
        } else if (cue_step == 0 && elapsed >= 400) {
            cue_step = 1;
            if (cue == Cue::Happy) motion.moveRelativeFrom(saved_angles, kTestHappySecond.yaw, kTestHappySecond.pitch, kTestHappySecond.speed);
        }
        const uint32_t finish_ms =
            cue == Cue::ShakeHead ? 2400 :
            cue == Cue::Nod ? 1350 :
            cue == Cue::Surprise ? 800 : 850;
        if (elapsed >= finish_ms &&
            (cue != Cue::ShakeHead || cue_step >= 4) &&
            (cue != Cue::Nod || cue_step >= 1)) {
            // Keep ownership until the return-to-start motion settles.
            // A bounded timeout avoids blocking all future actions if a
            // servo stops reporting movement completion.
            if ((cue == Cue::ShakeHead || cue == Cue::Nod) &&
                motion.isMoving() && elapsed < 5000) return;
            ESP_LOGI(LOG_TAG, "servo action finished: %s", cue_names[static_cast<int>(cue)]);
            cue_active = false;
            Release(sc, true);
            phase = Phase::Boot;
            schedule.Reset();
        }
    }
};
Runtime r;

struct SpeechEffectsRuntime {
    bool armed = false;
    bool active = false;
    bool has_emotion = false;
    bool owns_neck = false;
    bool motion_blocked = false;
    bool at_origin = true;
    avatar::Emotion emotion = avatar::Emotion::Neutral;
    SpeechEffectPreset preset = SpeechEffectFor(avatar::Emotion::Neutral);
    uitk::Vector2i origin{0, 0};
    uint32_t motion_epoch = 0, led_epoch = 0;
    uint32_t yaw_revision = 0, pitch_revision = 0;
    uint32_t changed_at = 0;
    unsigned nod_step = 0;

    void Arm(StackChan& sc) {
        *this = SpeechEffectsRuntime{};
        armed = true;
        motion_epoch = speech_motion_epoch.load();
        led_epoch = speech_led_epoch.load();
        yaw_revision = sc.motion().yawServo().getCommandRevision();
        pitch_revision = sc.motion().pitchServo().getCommandRevision();
    }

    bool SceneAvailable() const {
        return enabled.load() && !IsSleepSceneActive() && !IsWakeSequenceActive() &&
               !r.own_face;
    }

    bool MotionAvailable(StackChan& sc) {
        auto& motion = sc.motion();
        if (speech_motion_epoch.load() != motion_epoch || r.cue_active ||
            queued_cue.load() >= 0 ||
            !SceneAvailable() || r.own_neck || motion.isModifyLocked() || sc.avatar().isModifyLocked() ||
            (owns_neck && (yaw_revision != motion.yawServo().getCommandRevision() ||
                           pitch_revision != motion.pitchServo().getCommandRevision()))) {
            // A newer owner must never be undone by a delayed speech restore.
            motion_blocked = true;
            owns_neck = false;
            speech_tracking_paused.store(false);
        }
        return !motion_blocked;
    }

    void Target(StackChan& sc, int yaw, int pitch, int speed) {
        if (!MotionAvailable(sc)) return;
        auto& motion = sc.motion();
        motion.moveRelativeFrom(origin, yaw, pitch, speed);
        yaw_revision = motion.yawServo().getCommandRevision();
        pitch_revision = motion.pitchServo().getCommandRevision();
        at_origin = yaw == 0 && pitch == 0;
    }

    void Set(StackChan& sc, avatar::Emotion next, uint32_t now) {
        if (!active || (has_emotion && emotion == next)) return;
        has_emotion = true;
        emotion = next;
        preset = SpeechEffectFor(next);
        changed_at = now;
        nod_step = 0;
        if (speech_led_epoch.load() == led_epoch && SceneAvailable())
            GetHAL().setConversationRgbColor(preset.red, preset.green, preset.blue);
        if (MotionAvailable(sc)) {
            auto& motion = sc.motion();
            if (preset.speed == 0) {
                if (owns_neck && !at_origin) Target(sc, 0, 0, 200);
            } else {
                if (!owns_neck) {
                    speech_tracking_paused.store(true);
                    perception_look_pending.store(false);
                    origin = motion.getCurrentAngles();
                    owns_neck = true;
                    yaw_revision = motion.yawServo().getCommandRevision();
                    pitch_revision = motion.pitchServo().getCommandRevision();
                }
                Target(sc, preset.yaw, preset.pitch, preset.speed);
            }
        }
        ESP_LOGI(LOG_TAG, "[EMOTION_EFFECT] emotion=%d rgb=%u,%u,%u neck=%d",
                 static_cast<int>(next), static_cast<unsigned>(preset.red),
                 static_cast<unsigned>(preset.green), static_cast<unsigned>(preset.blue),
                 owns_neck && !motion_blocked);
    }

    void Tick(StackChan& sc, uint32_t now) {
        if (!active || !owns_neck || !MotionAvailable(sc)) return;
        const auto elapsed = now - changed_at;
        // Release the tracking hold as soon as the finite gesture is done.
        // The perception worker still avoids HTTP while audio is playing.
        const uint32_t finish_ms = preset.shake ? 2400 : preset.happy_sway ? 1250 :
            preset.nods ? preset.nods * 2 * preset.step_ms + 400 : 1200;
        if (elapsed >= finish_ms) {
            if (!at_origin) Target(sc, 0, 0, 200);
            owns_neck = false;
            speech_tracking_paused.store(false);
            ESP_LOGI(LOG_TAG, "[EMOTION_EFFECT] gesture complete; tracking released");
            return;
        }
        if (preset.shake) {
            if (nod_step < 4 && elapsed >= (nod_step + 1) * kTestShakeStepMs) {
                ++nod_step;
                const auto target = TestShakeTarget(nod_step);
                Target(sc, target.yaw, target.pitch, target.speed);
            }
            return;
        }
        if (preset.happy_sway) {
            const auto elapsed = now - changed_at;
            if (nod_step < 2 && elapsed >= 850) {
                nod_step = 2;
                Target(sc, 0, 0, 200);
            } else if (nod_step == 0 && elapsed >= preset.step_ms) {
                nod_step = 1;
                Target(sc, kTestHappySecond.yaw, kTestHappySecond.pitch, kTestHappySecond.speed);
            }
            return;
        }
        if (!preset.nods) return;
        const auto step = SpeechNodStep(preset.nods, now - changed_at, preset.step_ms);
        if (step == nod_step) return;
        nod_step = step;
        const bool home = SpeechNodAtOrigin(step);
        Target(sc, home ? 0 : preset.yaw, home ? 0 : preset.pitch, preset.speed);
    }

    void End(StackChan& sc) {
        if (active) {
            if (MotionAvailable(sc) && owns_neck && !at_origin)
                Target(sc, 0, 0, 200);
            if (speech_led_epoch.load() == led_epoch)
                GetHAL().releaseConversationRgbColor();
        }
        speech_tracking_paused.store(false);
        perception_look_pending.store(false);
        *this = SpeechEffectsRuntime{};
    }
};
SpeechEffectsRuntime speech_effects;

void DreamRemarkTick(StackChan& sc, const State& state, uint32_t now, bool interrupted_by_user) {
    if (dream_remark.motif == avatar_dream::Motif::None) return;
    const bool greeting = Application::GetInstance().IsAnnouncementActive();
    const bool allowed_phase = state.phase == Phase::NapWake || state.phase == Phase::Awake;
    const bool in_conversation = !idle.load() && !greeting;
    if (!allowed_phase || interrupted_by_user || in_conversation ||
        r.cue_active || r.conversation_pose != ConversationPose::None ||
        manual_sleep_requested.load() || quiet_mode::Active() ||
        dream_remark_cancel_requested.exchange(false)) {
        ESP_LOGI(LOG_TAG, "dream remark canceled: interrupted or new activity");
        CancelDreamRemark();
        return;
    }
    // The timed wake's normal announcement is always first. If the
    // configured greeting cannot start, do not leave a remark queued.
    if (!dream_remark.timer_requested) {
        if (state.phase != Phase::NapWake ||
            static_cast<uint32_t>(now - dream_remark.selected_at) > 15000)
            CancelDreamRemark();
        return;
    }
    if (voice::IsBusy()) {
        // The timer greeting is allowed to finish; any later speech during
        // the two-second remark pause takes priority and cancels the remark.
        if (dream_remark.due_at != 0) CancelDreamRemark();
        return;
    }
    // Wait for the actual subtitle bubble to disappear, not just the PCM
    // stream to finish, and then keep the full two-second S3 pause.
    const auto& bubble = sc.avatar().getKeyElements().speechBubble;
    if (bubble && bubble->getVisible()) {
        dream_remark.due_at = 0;
        return;
    }
    if (!dream_remark.due_at) {
        dream_remark.due_at = now + 2000;
        return;
    }
    if (static_cast<int32_t>(now - dream_remark.due_at) < 0) return;
    const char* words = avatar_dream::Remark(dream_remark.motif);
    const auto* cfg = voice::Current();
    if (!words || !cfg) {
        CancelDreamRemark();
        return;
    }
    if (voice::Request(*cfg, "dream_remark", words)) {
        ESP_LOGI(LOG_TAG, "[SPEECH] kind=dream_remark phase=requested motif=%s",
                 avatar_dream::Name(dream_remark.motif));
        CancelDreamRemark();
    } else if (static_cast<uint32_t>(now - dream_remark.due_at) > 15000) {
        ESP_LOGW(LOG_TAG, "dream remark abandoned: voice unavailable");
        CancelDreamRemark();
    }
}
}

void Start() {
    // Start after the StackChanAvatarDisplay has attached its avatar.
    quiet_mode::Init();
    voice::Init();
    environment::Init();
    voice::Reset();
    CancelDreamRemark();
    weather_changes = weather::Changes{};
    r = Runtime{};
    speech_effects = SpeechEffectsRuntime{};
    speech_tracking_paused.store(false);
    person_search = PersonSearch{};
    search_start.store(0); search_cancel.store(false); search_active.store(false);
    wake_pitch_pending.store(false);
    const Config config = LoadConfig();
    registered_wake_minute.store(config.wake_minute, std::memory_order_release);
    if (!r.schedule.SetConfig(config)) {
        ESP_LOGE(LOG_TAG, "stage-1 config rejected; using defaults");
    }
    ESP_LOGI(LOG_TAG, "config wake=%u sleep=%u theatre=%d naps=%d chance=%u",
             config.wake_minute, config.sleep_minute, config.theatre,
             config.naps, config.nap_chance);
    idle.store(false); interrupted.store(false);
    wake_intents.store(0); sleep_wake_eligible.store(false);
    wake_sequence_active.store(false);
    manual_motion.store(false); manual_led.store(false);
    queued_cue.store(-1); owns_auto.store(false);
    manual_sleep_requested.store(false);
    // Re-enable stage-1 runtime for the controlled audio/WebSocket A/B test.
    // Keep the scene redraw rate limit in place.
    enabled.store(true);
    // Called from the Xiaozhi main event task before regular wake-word audio.
    // One atomic wake intent drives the existing avatar action; the callback
    // returns only the configured utterance, not an HTTP endpoint.
    Application::GetInstance().SetWakeWordAnnouncementProvider([]() -> std::string {
        if (!enabled.load() || !sleep_wake_eligible.load() ||
            wake_sequence_active.load() || wake_intents.load() != 0) return {};
        // Web settings are saved under LVGL's lock. Snapshot the phrase
        // under that same lock so a concurrent edit cannot tear a UTF-8
        // utterance; the microphone/network code runs after unlocking.
        if (!lvgl_port_lock(100)) return {};
        const auto* settings = voice::Current();
        const std::string greeting = settings ? settings->wake_word : "";
        lvgl_port_unlock();
        if (greeting.empty() || !RequestWakeWord()) return {};
        return greeting;
    });
    status_logged = false;
    last_status_log_at = 0;
    ESP_LOGI(LOG_TAG, "runtime started enabled=true");
}
void Stop() {
    enabled.store(false);
    environment::SetIdle(false);
    Application::GetInstance().SetWakeWordAnnouncementProvider({});
    voice::Cancel();
    voice::Reset();
    voice::Stop();
    CancelDreamRemark();
    weather_changes = weather::Changes{};
    r.RestoreNightBacklight();
    r.ClearVisuals();
    r = Runtime{};
    speech_effects = SpeechEffectsRuntime{};
    speech_tracking_paused.store(false);
    person_search = PersonSearch{};
    search_start.store(0); search_cancel.store(false); search_active.store(false);
    wake_pitch_pending.store(false);
    queued_cue.store(-1);
    owns_auto.store(false);
    manual_sleep_requested.store(false);
    wake_intents.store(0); sleep_wake_eligible.store(false);
    wake_sequence_active.store(false);
}
// Caller holds the same LVGL lock used by Update(), so scene state is serialized.
Config GetConfig() { return r.schedule.GetConfig(); }
bool SetConfig(const Config& config) {
    if (!ValidConfig(config)) return false;
    if (!SaveConfig(config)) return false;
    if (!r.schedule.SetConfig(config)) return false;
    registered_wake_minute.store(config.wake_minute, std::memory_order_release);
    // Saving settings is not user activity; keep the current scene intact.
    ESP_LOGI(LOG_TAG, "stage-1 config saved and applied");
    return true;
}
void OnStandby(bool ready) {
    idle.store(ready);
    if (!ready && !wake_sequence_active.load()) voice::Cancel();
    if (!ready) {
        if (owns_auto.load()) ESP_LOGW(LOG_TAG, "[SLEEP_DIAG] interrupt source=standby_off");
        interrupted.store(true);
    }
}
void NotifyExternalSpeech() {
    // Xiaozhi owns the speech bubble once any ASR/system/TTS text arrives.
    // The end of a wake animation must never delete a newer subtitle.
    r.own_speech = false;
}
bool RequestWakeTouch(const char* source) {
    if (!enabled.load()) return false;
    if (wake_sequence_active.load()) {
        dream_remark_cancel_requested.store(true);
        return true; // consume repeated contact
    }
    if (!sleep_wake_eligible.load()) return false;
    ESP_LOGI(LOG_TAG, "[SLEEP_DIAG] wake_request source=%s kind=touch", source ? source : "unknown");
    wake_intents.fetch_or(1);
    return true;
}
bool RequestWakeWord() {
    if (!enabled.load()) return false;
    if (wake_sequence_active.load()) {
        dream_remark_cancel_requested.store(true);
        return true;
    }
    if (!sleep_wake_eligible.load()) return false;
    wake_intents.fetch_or(2);
    return true;
}
bool IsWakeSequenceActive() {
    return wake_sequence_active.load() || wake_intents.load() != 0;
}
bool IsSleepWakeEligible() {
    return sleep_wake_eligible.load();
}
bool IsNapSleepActive() {
    return enabled.load() && r.phase == Phase::NapSleep;
}
bool IsNightBedtimeMessageActive() {
    return enabled.load() && r.phase == Phase::NightMessage;
}
bool IsSleepSceneActive() {
    return enabled.load() && (Runtime::SleepPreparing(r.phase) || Runtime::Sleeping(r.phase));
}
void CancelForExternalChange(StackChan& sc, const char* source) {
    if (!enabled.load()) return;
    if (IsSleepSceneActive() || r.phase == Phase::NightTemporary || r.phase == Phase::NightPending)
        ESP_LOGW(LOG_TAG, "[SLEEP_DIAG] reset source=external:%s phase=%d standby=%d",
                 source ? source : "unknown", static_cast<int>(r.phase), idle.load());
    r.EndConversation(sc);
    CancelDreamRemark();
    r.RestoreNightBacklight();
    r.Release(sc, true);
    r.cue_active = false;
    queued_cue.store(-1); // New explicit motion must not be followed by a stale gesture.
    r.phase = Phase::Boot;
    r.schedule.Reset();
    interrupted.store(true); // Reset idle deadlines even if the caller is a touch event.
}
void NotifyManualMotion() {
    speech_motion_epoch.fetch_add(1);
    if (owns_auto.load()) ESP_LOGW(LOG_TAG, "[SLEEP_DIAG] interrupt source=manual_motion");
    manual_motion.store(true); dream_remark_cancel_requested.store(true); interrupted.store(true);
}
void NotifyManualLed() {
    speech_led_epoch.fetch_add(1);
    if (owns_auto.load()) ESP_LOGW(LOG_TAG, "[SLEEP_DIAG] interrupt source=manual_led");
    manual_led.store(true); dream_remark_cancel_requested.store(true); interrupted.store(true);
}
void PrepareSpeechExpression(StackChan& sc, const char* source) {
    // An explicit voice gesture may arrive before the first TTS status or
    // another emotion. Keep it queued/active while releasing the old face.
    if (r.cue_active || queued_cue.load() >= 0) {
        r.EndConversation(sc);
        return;
    }
    CancelForExternalChange(sc, source);
}
void BeginSpeechTurn(StackChan& sc) { speech_effects.Arm(sc); }
void StartSpeechEffects(StackChan& sc, avatar::Emotion emotion) {
    if (speech_effects.active) return;
    person_search.Pause(sc);
    if (!speech_effects.armed) speech_effects.Arm(sc);
    speech_effects.active = true;
    speech_effects.Set(sc, emotion, GetHAL().millis());
}
void UpdateSpeechEffects(StackChan& sc, avatar::Emotion emotion) {
    speech_effects.Set(sc, emotion, GetHAL().millis());
}
void EndSpeechEffects(StackChan& sc) { speech_effects.End(sc); }

void SetConversationPose(StackChan& sc, ConversationPose pose) {
    if (!enabled.load() || !sc.hasAvatar()) return;
    CancelDreamRemark();
    r.BeginConversation(sc, pose, GetHAL().millis());
}
void BeginListeningComplete(StackChan& sc) {
    if (!enabled.load() || !sc.hasAvatar() ||
        r.conversation_pose != ConversationPose::Listening) return;
    r.BeginConversation(sc, ConversationPose::Thinking, GetHAL().millis());
    ESP_LOGI(LOG_TAG, "S3 face: listening nod -> thinking");
}
bool IsConversationFaceActive() {
    return r.conversation_pose != ConversationPose::None;
}
bool IsAutonomousSceneActive() { return owns_auto.load(); }
bool IsIdleServoMotionBlocked() {
    // Suppress ordinary random head movement for the entire scene and
    // at least 3000ms from theatre start, even for 2.8s skits.
    const uint32_t until = r.theatre_motion_protect_until;
    return owns_auto.load() || voice::IsBusy() || VoicePending() ||
           dream_remark.motif != avatar_dream::Motif::None ||
           (until != 0 && static_cast<int32_t>(GetHAL().millis() - until) < 0);
}
bool RequestSleep() {
    if (!enabled.load()) return false;
    manual_sleep_requested_at.store(GetHAL().millis());
    manual_sleep_requested.store(true);
    ESP_LOGI(LOG_TAG, "manual sleep queued until conversation playback completes");
    return true;
}
bool RequestCue(const char* id) {
    if (!id || !enabled.load() || owns_auto.load()) return false;
    for (int i = 0; i < static_cast<int>(Cue::Count); ++i) {
        if (std::strcmp(id, cue_names[i]) != 0) continue;
        int empty = -1;
        // Refuse overlapping requests rather than replacing a queued action.
        if (!queued_cue.compare_exchange_strong(empty, i)) return false;
        speech_motion_epoch.fetch_add(1);
        return true;
    }
    return false;
}

bool IsSpeechTrackingPaused() { return speech_tracking_paused.load(); }
void RequestWakeSearchPitch() { wake_pitch_pending.store(true); }
void StartPersonSearch(int yaw_delta) {
    search_cancel.store(false);
    search_active.store(true);
    search_start_epoch.store(speech_motion_epoch.load());
    search_start.store(yaw_delta == 0 ? 1001 : std::clamp(yaw_delta, -700, 700));
}
void StopPersonSearch() {
    wake_pitch_pending.store(false);
    search_start.store(0);
    search_cancel.store(true);
    search_active.store(false);
}
bool IsPersonSearchActive() { return search_active.load(); }
bool IsPersonSearchStopping() { return search_cancel.load(); }


bool RequestPerceptionLook(int yaw_delta, int pitch_delta, bool direct)
{
    constexpr int kMaxYawDelta   = 900;
    constexpr int kMaxPitchDelta = 450;
    yaw_delta                    = std::clamp(yaw_delta, -kMaxYawDelta, kMaxYawDelta);
    pitch_delta                  = std::clamp(pitch_delta, -kMaxPitchDelta, kMaxPitchDelta);
    if (yaw_delta == 0 && pitch_delta == 0 && !direct) return false;
    perception_yaw_delta.store(yaw_delta, std::memory_order_relaxed);
    perception_pitch_delta.store(pitch_delta, std::memory_order_relaxed);
    perception_requested_at.store(GetHAL().millis(), std::memory_order_relaxed);
    if (speech_tracking_paused.load() || search_active.load() || search_cancel.load()) return false;
    perception_look_direct.store(direct, std::memory_order_relaxed);
    perception_look_pending.store(true, std::memory_order_release);
    return true;
}

void Update(StackChan& sc) {
    if (!enabled.load() || !sc.hasAvatar()) return;
    const uint32_t now = GetHAL().millis();
    quiet_mode::Tick();
    // Evaluate the monologue deadline exactly once, after the actual
    // event/scene decision (including the early-return conversation paths).
    const auto tick_monologue = [&](Phase phase, bool available) {
        if (const auto* config = voice::Current())
            voice::TickMonologue(*config, phase, available, now);
    };
    // While sleeping, the quiet mark is part of the rest face and must not
    // disappear just because an announcement/transport becomes busy.
    // Explicit wake and conversation still control their own overlay.
    const bool quiet_sleep = quiet_mode::Active() && Runtime::Sleeping(r.phase) &&
                             !wake_sequence_active.load();
    if (!quiet_sleep &&
        (!quiet_mode::Active() || !idle.load() || voice::IsBusy() ||
         VoicePending() || r.conversation_pose != ConversationPose::None ||
         wake_sequence_active.load()))
        r.SyncQuietMark(sc, false);
    const bool environment_idle = idle.load() && !voice::IsBusy() && !VoicePending() &&
                                  r.conversation_pose == ConversationPose::None &&
                                  !r.cue_active && !wake_sequence_active.load();
    environment::SetIdle(environment_idle);
    // Weather-change detection shares the existing one-second environment
    // tick; it never adds a fetch, worker or independent speech scheduler.
    const bool weather_check_due =
        r.environment_poll_at == 0 || now - r.environment_poll_at >= 1000;
    if (weather_check_due) {
        r.environment_poll_at = now;
        environment::Poll();
    }
    const auto poll_weather_change = [&](bool active, bool available) {
        if (!weather_check_due) return false;
        const int64_t time_us = esp_timer_get_time();
        weather_changes.Observe(environment::ReadAppearance(), time_us, active);
        if (!available || !weather_changes.Ready(time_us)) return false;
        const auto* config = voice::Current();
        // The existing monologue transport already honors quiet-mode and
        // playback exclusion in both Stage1 and Xiaozhi.
        if (!config || !voice::Request(*config, "monologue", weather_changes.Text()))
            return false;
        weather_changes.Accepted(time_us);
        ESP_LOGI(LOG_TAG, "[WEATHER_CHANGE] phase=requested transport=monologue text=%s",
                 weather_changes.Text());
        return true;
    };
    bool event = interrupted.exchange(false);
    const uint8_t wake_bits = wake_intents.exchange(0);
    const WakeReason requested = (wake_bits & 2) ? WakeReason::Word :
                                  (wake_bits & 1) ? WakeReason::Touch : WakeReason::None;
    const bool accepted_wake = requested != WakeReason::None &&
                                r.schedule.RequestWake(requested, now);
    if (requested != WakeReason::None) dream_remark_cancel_requested.store(true);
    if (accepted_wake || wake_sequence_active.load()) event = false;
    if (manual_motion.exchange(false)) {
        r.motion_held = true;
        // A newer manual command takes precedence over ongoing animations.
        if (r.cue_active) r.restore_neck_on_release = false;
        queued_cue.store(-1);
    }
    if (manual_led.exchange(false)) r.led_held = true;
    if (event && (r.own_face || r.cue_active)) {
        if (IsSleepSceneActive() || r.phase == Phase::NightTemporary || r.phase == Phase::NightPending)
            ESP_LOGW(LOG_TAG, "[SLEEP_DIAG] reset source=interrupt phase=%d standby=%d wake_bits=%u",
                     static_cast<int>(r.phase), idle.load(), static_cast<unsigned>(wake_bits));
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
    // Handle search before ordinary corrections, so detection cancels the
    // sweep even when the detected face needs no positional correction.
    auto& ps = person_search;
    auto& search_motion = sc.motion();
    auto& search_yaw = search_motion.yawServo();
    const auto search_limits = search_yaw.getAngleLimit();
    const auto device_state = Application::GetInstance().GetDeviceState();
    const bool search_blocked = device_state == kDeviceStateSpeaking || speech_tracking_paused.load() ||
        r.cue_active || r.own_neck || IsWakeSequenceActive() || Runtime::Sleeping(r.phase) ||
        Runtime::SleepPreparing(r.phase) || search_motion.isModifyLocked() || sc.avatar().isModifyLocked();
    if (search_cancel.load()) {
        perception_look_pending.store(false);
        ps.End(sc, true);
        search_cancel.store(false);
    }
    if (!search_blocked && wake_pitch_pending.exchange(false)) {
        const auto pitch_limits = search_motion.pitchServo().getAngleLimit();
        const int target = std::clamp(100, pitch_limits.x, pitch_limits.y);
        search_motion.movePitchWithSpeed(target, 150);
        ESP_LOGI(LOG_TAG, "[PERSON_SEARCH] wake pitch=%d", target);
    }
    const int start = search_blocked ? 0 : search_start.exchange(0);
    if (start != 0) {
        ps.End(sc, true);
        const int delta = start == 1001 ? 0 : start;
        perception_look_pending.store(false);
        ps.running = true;
        ps.direction = delta < 0 ? -1 : 1;
        ps.origin = std::clamp(search_motion.getCurrentYawAngle() + delta, search_limits.x, search_limits.y);
        ps.target = ps.direction > 0 ? search_limits.y : search_limits.x;
        ps.epoch = search_start_epoch.load();
        search_active.store(true);
        ESP_LOGI(LOG_TAG, "[PERSON_SEARCH] start direction=%d target=%d return=%d limits=%d,%d",
                 ps.direction, ps.target, ps.origin, search_limits.x, search_limits.y);
    }
    if (ps.running) {
        if (ps.epoch != speech_motion_epoch.load()) {
            ps.End(sc, false); // manual/explicit command now owns the neck
        } else if (search_blocked) {
            ps.Pause(sc);
        } else {
            if (ps.commanded && search_yaw.getCommandRevision() != ps.revision) {
                ps.End(sc, false); // stall protection or another owner
            } else if (ps.commanded) {
                const int current = search_yaw.getCurrentAngle();
                // Keep the full-limit motion target, but reverse 10 degrees
                // before it so the spring does not settle at the endpoint.
                constexpr int kSearchTurnMargin = 100; // 0.1-degree units
                const bool sweep = ps.phase != PersonSearch::Return;
                const int direction = ps.phase == PersonSearch::First ? ps.direction : -ps.direction;
                const int margin = std::min(kSearchTurnMargin, (search_limits.y - search_limits.x) / 2);
                const bool reached = sweep
                    ? (direction > 0 ? current >= search_limits.y - margin
                                     : current <= search_limits.x + margin)
                    : std::abs(current - ps.target) <= 5;
                if (reached || !search_yaw.isMoving()) {
                    if (ps.phase == PersonSearch::Return) {
                        ESP_LOGI(LOG_TAG, "[PERSON_SEARCH] complete: no person");
                        ps.End(sc, false);
                    } else {
                        if (ps.phase == PersonSearch::First) {
                            ps.phase = PersonSearch::Reverse;
                            ps.target = ps.direction > 0 ? search_limits.x : search_limits.y;
                        } else {
                            ps.phase = PersonSearch::Return;
                            ps.target = ps.origin;
                        }
                        ps.commanded = false;
                        ESP_LOGI(LOG_TAG, "[PERSON_SEARCH] phase=%d yaw=%d target=%d", ps.phase, current, ps.target);
                    }
                }
            }
            // Initial move and each reversal use the same command path.
            // Issue the next target in this update, without a wait state.
            if (ps.running && !ps.commanded && !search_cancel.load()) {
                ps.target = std::clamp(ps.target, search_limits.x, search_limits.y);
                search_motion.moveYawWithSpeed(ps.target, 150);
                ps.revision = search_yaw.getCommandRevision();
                ps.commanded = true;
            }
        }
    }
    if (perception_look_pending.load(std::memory_order_acquire)) {
        const bool expired = static_cast<uint32_t>(now - perception_requested_at.load()) > 3000;
        // Wake-triggered tracking is allowed to replace the wake animation's
        // neck target; sleep and explicit skits retain priority.
        const bool motion_available = device_state != kDeviceStateSpeaking && !ps.running && !speech_tracking_paused.load() && !r.cue_active && !Runtime::Sleeping(r.phase) &&
                                      !Runtime::SleepPreparing(r.phase);
        if (expired) {
            perception_look_pending.store(false, std::memory_order_release);
            ESP_LOGW(LOG_TAG, "perception look expired while motion was busy");
        } else if (motion_available && perception_look_pending.exchange(false)) {
            auto& motion           = sc.motion();
            auto& yaw              = motion.yawServo();
            auto& pitch            = motion.pitchServo();
            const int yaw_before   = yaw.getCurrentAngle();
            const int pitch_before = pitch.getCurrentAngle();
            const auto yaw_limit   = yaw.getAngleLimit();
            const auto pitch_limit = pitch.getAngleLimit();
            const int yaw_target   = std::clamp(yaw_before + perception_yaw_delta.load(), yaw_limit.x, yaw_limit.y);
            const int pitch_target =
                std::clamp(pitch_before + perception_pitch_delta.load(), pitch_limit.x, pitch_limit.y);
            ESP_LOGI(LOG_TAG, "perception look yaw=%d->%d pitch=%d->%d", yaw_before, yaw_target, pitch_before,
                     pitch_target);
            // ウェイク後は相手を待たせないよう、通常の小芝居より素早く向く。
            if (perception_look_direct.load(std::memory_order_relaxed)) {
                // Follow a bounded correction over the existing camera frame interval.
                // SCS position mode supports a travel time; do not use unbounded PWM.
                constexpr int kFaceMaxSpeed = 300;
                constexpr uint32_t kFaceFrameMs = 250;
                const int distance = std::max(std::abs(yaw_target - yaw_before),
                                              std::abs(pitch_target - pitch_before));
                // 0.3125 degrees per raw step, angle units are tenths of a degree.
                const uint32_t speed_limited_ms =
                    (static_cast<uint32_t>(distance) * 320 + kFaceMaxSpeed - 1) / kFaceMaxSpeed;
                const uint32_t duration = std::max(kFaceFrameMs, speed_limited_ms);
                motion.moveDirect(yaw_target, pitch_target, kFaceMaxSpeed, distance == 0 ? 0 : duration);
            } else {
                motion.moveWithSpeed(yaw_target, pitch_target, 1000);
            }
        }
    }
    if (speech_effects.active) {
        if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking)
            speech_effects.Tick(sc, now);
        else
            speech_effects.End(sc);
    }
    if (r.cue_active) {
        r.schedule.DiscardDueRandomEvents(now, esp_random());
        tick_monologue(r.phase, false);
        poll_weather_change(false, false);
        r.SyncQuietMark(sc, false);
        r.TickCue(sc, now);
        r.SyncWeather(sc, now, true);
        return;
    }
    // Explicit conversation state always wins over idle skits and sleep
    // previews; it is released on SPEAKING/STANDBY or other status changes.
    if (r.conversation_pose != ConversationPose::None) {
        r.schedule.DiscardDueRandomEvents(now, esp_random());
        tick_monologue(r.phase, false);
        poll_weather_change(false, false);
        r.SyncQuietMark(sc, false);
        r.DrawConversation(sc, now);
        r.SyncWeather(sc, now, false);
        return;
    }

    std::time_t epoch = std::time(nullptr);
    std::tm local{};
    const bool valid = epoch > 1700000000 && localtime_r(&epoch, &local) && local.tm_year >= 124;
    const auto minute = static_cast<uint16_t>(valid ? local.tm_hour * 60 + local.tm_min : 0);
    const bool standby = idle.load();
    // Xiaozhi can already be CONNECTING/LISTENING after a wake word.
    // Preserve the brief S3 wake animation without blocking microphone I/O.
    const bool pose_standby = standby || accepted_wake || wake_sequence_active.load();
    const bool linked_event_pending = dream_remark.motif != avatar_dream::Motif::None ||
                                      manual_sleep_requested.load();
    const bool sleep_home_ready = r.SleepHomeReady(sc, now);
    State state = r.schedule.Step({now,minute,valid,pose_standby,event,esp_random(),
                                   voice::IsBusy() || VoicePending(),
                                   linked_event_pending, sleep_home_ready});
    // Keep S3's explicit "寝て良いよ" separate from temporary quiet mode.
    // Never turn off the face/neck while the Live reply is still speaking.
    if (manual_sleep_requested.load()) {
        if (static_cast<uint32_t>(now - manual_sleep_requested_at.load()) > 60000) {
            manual_sleep_requested.store(false);
            ESP_LOGW(LOG_TAG, "manual sleep request expired before standby");
        } else if (valid && standby && !event && !accepted_wake &&
                   !wake_sequence_active.load() && !voice::IsBusy() &&
                   !VoicePending() && wake_intents.load() == 0 &&
                   !r.cue_active && r.conversation_pose == ConversationPose::None &&
                   r.schedule.RequestSleep(now, minute, true)) {
            manual_sleep_requested.store(false);
            state = r.schedule.Step({now,minute,true,true,false,esp_random(),false,false,sleep_home_ready});
            ESP_LOGI(LOG_TAG, "manual sleep started: phase=%d", static_cast<int>(state.phase));
        }
    }
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
    const Phase old_phase = r.phase;
    // Snapshot the last *displayed* motif before Begin() discards the old
    // dream scene. Never link a non-timer wake or an unshown motif.
    if (old_phase == Phase::NapSleep && state.phase == Phase::NapWake &&
        state.wake_reason == WakeReason::Timer) {
        CancelDreamRemark();
        const auto dream = avatar_dream::Last();
        if (dream != avatar_dream::Motif::None && !quiet_mode::Active() &&
            voice::Current() && voice::Current()->timer[0] &&
            esp_random() % 100 < 30) {
            dream_remark.motif = dream;
            dream_remark.selected_at = now;
            ESP_LOGI(LOG_TAG, "dream remark selected: motif=%s", avatar_dream::Name(dream));
        }
    }
    if (Runtime::SleepYawn(old_phase) && Runtime::Sleeping(state.phase)) {
        if (sleep_home_ready)
            ESP_LOGI(LOG_TAG, "sleep prepare: movement finished before sleep");
        else
            ESP_LOGW(LOG_TAG, "sleep prepare: settle timeout; no further home command");
    }
    r.Begin(sc, state.phase, state.kind, now);
    VoiceTick(old_phase, state, now);
    const bool monologue_eligible = valid && standby && !event && !accepted_wake &&
        !wake_sequence_active.load() && !voice::IsBusy() && !VoicePending() &&
        dream_remark.motif == avatar_dream::Motif::None && !manual_sleep_requested.load() &&
        state.phase == Phase::Awake && old_phase == Phase::Awake;
    // Weather updates during night, nap, quiet or explicit transitions are
    // folded into the baseline, never delivered later as stale alerts.
    const bool weather_active = valid && state.phase == Phase::Awake &&
        old_phase == Phase::Awake && !quiet_mode::Active() && !event &&
        !accepted_wake && !wake_sequence_active.load() && !manual_sleep_requested.load();
    const bool weather_announced = poll_weather_change(weather_active, monologue_eligible);
    tick_monologue(state.phase, monologue_eligible && !weather_announced);
    DreamRemarkTick(sc, state, now, event && !Application::GetInstance().IsAnnouncementActive());
    r.TickNightBacklight(now);
    if (state.phase == Phase::NapWake || state.phase == Phase::NightWake ||
        state.phase == Phase::MorningWake)
        r.TickWake(sc, state.phase_elapsed_ms);
    if (Runtime::SleepYawn(state.phase)) r.TickSleepYawn(sc, state.phase_elapsed_ms, now);
    sleep_wake_eligible.store(state.phase == Phase::NapSleep ||
                              state.phase == Phase::NightSleep ||
                              state.phase == Phase::NapYawn ||
                              state.phase == Phase::NightYawn);
    // Limit scene redraws: the app update loop may run faster than animations
    // need, while each render mutates multiple LVGL objects.
    const uint32_t draw_interval_ms = voice::SleepSoundPlaying() ? 30 : 100;
    if (r.own_face && (r.last_draw_at == 0 || now - r.last_draw_at >= draw_interval_ms)) {
        r.Draw(sc, state);
        r.ApplyTheatreFace(sc, state);
        r.last_draw_at = now;
    }
    // During sleep, keep the quiet decoration attached to the rest face
    // regardless of speech/transport activity. Outside sleep retain the
    // original rule: hide during conversation, actions and playback.
    // Render after face updates so it follows the real mouth position.
    const bool quiet_active = quiet_mode::Active();
    const bool sleeping_quiet = quiet_active && Runtime::Sleeping(state.phase) &&
                                !wake_sequence_active.load();
    r.SyncQuietMark(sc, sleeping_quiet ||
        (quiet_active && standby && !r.cue_active &&
         r.conversation_pose == ConversationPose::None &&
         !voice::IsBusy() && !VoicePending() && !r.own_speech &&
         !wake_sequence_active.load()));
    // Autonomous faces still own their full expression. Ordinary speech only
    // suppresses the cold scarf when it physically overlaps the bubble; the
    // top-left badge and top-right heat drops remain visible.
    r.SyncWeather(sc, now, r.own_face || Runtime::Sleeping(state.phase));
}

// Called by the firmware's existing WebSocket transport via a weak hook.
// Schedule is defined by the registered local wake-up clock, not the time
// the robot actually woke or whether a morning greeting was played.
bool InRegisteredWakeWindow()
{
    if (!enabled.load(std::memory_order_acquire)) return false;
    const std::time_t now = std::time(nullptr);
    if (now < 1700000000) return false;
    std::tm local{};
    if (!localtime_r(&now, &local) || local.tm_year < 124) return false;
    constexpr int kDaySeconds = 24 * 60 * 60;
    constexpr int kGroggySeconds = 15 * 60;
    const int current_second = local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    const int wake_second = registered_wake_minute.load(std::memory_order_acquire) * 60;
    return (current_second - wake_second + kDaySeconds) % kDaySeconds < kGroggySeconds;
}
} // namespace stackchan::stage1

extern "C" bool stackchan_wake_groggy_active()
{
    return stackchan::stage1::InRegisteredWakeWindow();
}
