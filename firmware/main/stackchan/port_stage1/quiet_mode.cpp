#include "quiet_mode.h"
#include <application.h>
#include <nvs.h>
#include <esp_log.h>
#include <mutex>

extern "C" void stackchan_nvs_commit_observer(void);

namespace stackchan::stage1::quiet_mode {
namespace {
constexpr char kNamespace[] = "sc_quiet";
constexpr char kSettingsKey[] = "settings_v1";
constexpr char kTag[] = "STAGE1-QUIET";
std::mutex state_mutex, write_mutex;
Settings settings;
bool observed_active = false;
void ObserveLocked(bool voice_command) {
    const bool active = Evaluate(settings, std::time(nullptr)).active;
    if (active == observed_active) return;
    observed_active = active;
    ESP_LOGI(kTag, "[QUIET] active=%d", active);
    // Only schedules work; no network or LVGL operation under this lock.
    if (active) Application::GetInstance().QuietModeStarted(voice_command);
}
}

void Init() {
    std::lock_guard<std::mutex> writer(write_mutex);
    Settings loaded;
    nvs_handle_t handle;
    const esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = sizeof(loaded);
        const auto read = nvs_get_blob(handle, kSettingsKey, &loaded, &size);
        if (read == ESP_ERR_NVS_NOT_FOUND) {
            loaded = {};
            nvs_get_u32(handle, "until", &loaded.temporary_until);
        } else if (read != ESP_OK || size != sizeof(loaded) || !Valid(loaded)) {
            ESP_LOGW(kTag, "Invalid stored quiet settings");
            loaded = {};
        }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(kTag, "NVS read failed: %s", esp_err_to_name(err));
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    settings = loaded;
    observed_active = false;
}
Status GetStatus() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return Evaluate(settings, std::time(nullptr));
}
bool Active() { return GetStatus().active; }
uint32_t RemainingSeconds() {
    const auto status = GetStatus();
    return status.active ? status.until - status.now : 0;
}
void Tick() {
    std::lock_guard<std::mutex> lock(state_mutex);
    ObserveLocked(false);
}
esp_err_t Apply(const Request& request, bool voice_command) {
    const bool requires_clock = request.action != Action::Cancel &&
        !(request.action == Action::Schedule && !request.enabled);
    std::lock_guard<std::mutex> writer(write_mutex);
    const int64_t now = std::time(nullptr);
    if (requires_clock && !ClockReady(now)) return ESP_ERR_INVALID_STATE;
    Settings next;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        next = settings;
    }
    if (!Modify(next, request, now) || !Valid(next)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    auto err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, kSettingsKey, &next, sizeof(next));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "NVS save failed: %s", esp_err_to_name(err));
        return err;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        settings = next;
        ObserveLocked(voice_command);
    }
    stackchan_nvs_commit_observer();
    return ESP_OK;
}
bool SetMinutes(int minutes) {
    if (minutes < 0 || minutes > 1440) return false;
    Request request;
    request.action = minutes ? Action::Minutes : Action::Cancel;
    request.minutes = minutes;
    return Apply(request) == ESP_OK;
}
} // namespace stackchan::stage1::quiet_mode
