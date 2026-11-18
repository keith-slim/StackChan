#include "voice_client.h"
#include <application.h>
#include <esp_log.h>
#include <cstring>

namespace stackchan::stage1::voice {
namespace {
constexpr char TAG[] = "STAGE1-VOICE";
}

bool IsBusy() {
    return Application::GetInstance().IsAnnouncementActive();
}

uint8_t SleepSoundPlaying() {
    return Application::GetInstance().ActiveSleepAnnouncement();
}

void Cancel() {
    // Only an interrupted snore/sleep-talk may be canceled here. Never
    // terminate a scheduled greeting, ordinary conversation, or Web test.
    Application::GetInstance().CancelSleepAnnouncement();
}

bool Request(const Settings& config, const char* kind, const char* text) {
    (void)config; // Voice uses Xiaozhi's existing WebSocket server on 12800.
    if (!kind || !text || !text[0] || std::strlen(text) > 512) return false;
    const bool queued = Application::GetInstance().RequestAnnouncement(kind, text);
    ESP_LOGI(TAG, "[SPEECH] kind=%s phase=%s transport=ws12800",
             kind, queued ? "requested" : "busy_or_unavailable");
    return queued;
}
} // namespace stackchan::stage1::voice
