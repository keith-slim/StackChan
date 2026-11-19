#include "voice_client.h"
#include "quiet_mode.h"
#include <application.h>
#include <esp_log.h>
#include <esp_random.h>
#include <cstring>

namespace stackchan::stage1::voice {
namespace {
constexpr char TAG[] = "STAGE1-VOICE";
Kind pending_scene = Kind::None;
Kind active_sleep = Kind::None;
Phase scene_phase = Phase::Boot;
uint32_t scene_since = 0, next_snore_at = 0, next_talk_at = 0;

const char* Tag(Kind kind) {
    switch (kind) {
    case Kind::Bedtime: return "bedtime";
    case Kind::Morning: return "morning";
    case Kind::Touch: return "touch";
    case Kind::Timer: return "timer";
    case Kind::Snore: return "snore";
    case Kind::SleepTalk: return "sleep_talk";
    default: return "unknown";
    }
}
const char* Phrase(const Settings& config, Kind kind) {
    switch (kind) {
    case Kind::Bedtime: return config.bedtime;
    case Kind::Morning: return config.morning;
    case Kind::Touch: return config.touch;
    case Kind::Timer: return config.timer;
    default: return nullptr;
    }
}
uint32_t Delay(uint16_t lo, uint16_t hi) {
    return (static_cast<uint32_t>(lo) + esp_random() % (static_cast<uint32_t>(hi - lo) + 1)) * 1000;
}
void Rearm(const Settings& config, Kind kind, uint32_t now) {
    if (kind == Kind::Snore)
        next_snore_at = now + Delay(config.snore_min, config.snore_max);
    else if (kind == Kind::SleepTalk)
        next_talk_at = now + Delay(config.talk_min, config.talk_max);
}
}

bool IsBusy() {
    return Application::GetInstance().IsAnnouncementActive();
}
uint8_t SleepSoundPlaying() {
    return Application::GetInstance().ActiveSleepAnnouncement();
}
void Cancel() {
    // Only snore/sleep-talk can be canceled. Leave greetings and conversation.
    Application::GetInstance().CancelSleepAnnouncement();
}
bool Request(const Settings& config, const char* kind, const char* text) {
    (void)config;
    if (!kind || !text || !text[0] || std::strlen(text) > 512) return false;
    // Only autonomous announcements are silenced. Touch/wake-word and
    // explicitly requested speech must continue to work in quiet mode.
    const bool autonomous = std::strcmp(kind, "snore") == 0 ||
                            std::strcmp(kind, "sleep_talk") == 0 ||
                            std::strcmp(kind, "dream_remark") == 0 ||
                            std::strcmp(kind, "bedtime") == 0 ||
                            std::strcmp(kind, "morning") == 0 ||
                            std::strcmp(kind, "timer") == 0;
    if (autonomous && quiet_mode::Active()) {
        ESP_LOGI(TAG, "[SPEECH] kind=%s phase=suppressed_quiet", kind);
        return false;
    }
    const bool queued = Application::GetInstance().RequestAnnouncement(kind, text);
    ESP_LOGI(TAG, "[SPEECH] kind=%s phase=%s transport=ws12800",
             kind, queued ? "requested" : "busy_or_unavailable");
    return queued;
}
void Reset() {
    pending_scene = active_sleep = Kind::None;
    scene_phase = Phase::Boot;
    scene_since = next_snore_at = next_talk_at = 0;
}
void SceneChanged(Phase phase, uint32_t now) {
    pending_scene = Kind::None;
    scene_phase = phase;
    scene_since = now;
}
void QueueScene(Kind kind) {
    pending_scene = kind;
}
bool Pending() { return pending_scene != Kind::None; }

bool TickScene(const Settings& config, Phase phase, uint32_t now) {
    if (pending_scene == Kind::None || scene_phase != phase) return false;
    const char* phrase = Phrase(config, pending_scene);
    if (phrase && !IsBusy() && Request(config, Tag(pending_scene), phrase)) {
        const bool timer = pending_scene == Kind::Timer;
        pending_scene = Kind::None;
        return timer;
    }
    if (static_cast<uint32_t>(now - scene_since) > 15000) {
        ESP_LOGW(TAG, "[SPEECH] kind=%s phase=timed_out_waiting_for_idle", Tag(pending_scene));
        pending_scene = Kind::None;
    }
    return false;
}
void EnterNap(const Settings& config, uint32_t now) {
    active_sleep = Kind::None;
    next_snore_at = next_talk_at = 0;
    Rearm(config, Kind::Snore, now);
    Rearm(config, Kind::SleepTalk, now);
}
void LeaveNap() {
    Cancel();
    active_sleep = Kind::None;
    next_snore_at = next_talk_at = 0;
}
void Quiet() {
    pending_scene = Kind::None;
    next_snore_at = next_talk_at = 0;
    if (active_sleep != Kind::None) Cancel();
    active_sleep = Kind::None;
}
void ResumeNap(const Settings& config, uint32_t now) {
    if (next_snore_at == 0 && next_talk_at == 0 && active_sleep == Kind::None)
        EnterNap(config, now);
}
bool SleepActive() { return active_sleep != Kind::None; }
Kind FinishSleepIfIdle(const Settings& config, uint32_t now) {
    if (active_sleep == Kind::None || IsBusy()) return Kind::None;
    const Kind completed = active_sleep;
    Rearm(config, completed, now);
    active_sleep = Kind::None;
    return completed; // Completion and transport failure both rearm the interval.
}
bool Due(Kind kind, uint32_t now) {
    const uint32_t due = kind == Kind::Snore ? next_snore_at :
                         kind == Kind::SleepTalk ? next_talk_at : 0;
    return due && static_cast<int32_t>(now - due) >= 0;
}
bool RequestSleep(Kind kind, const Settings& config, const char* text, uint32_t now) {
    if (kind != Kind::Snore && kind != Kind::SleepTalk) return false;
    if (Request(config, Tag(kind), text)) {
        active_sleep = kind;
        if (kind == Kind::Snore) next_snore_at = 0;
        else next_talk_at = 0;
        return true;
    }
    Rearm(config, kind, now);
    return false;
}
} // namespace stackchan::stage1::voice
