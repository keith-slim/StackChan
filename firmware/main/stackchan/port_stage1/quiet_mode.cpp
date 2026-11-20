#include "quiet_mode.h"
#include <nvs.h>
#include <esp_log.h>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <limits>

extern "C" void stackchan_nvs_commit_observer(void);

namespace stackchan::stage1::quiet_mode {
namespace {
constexpr char kNamespace[] = "sc_quiet";
constexpr char kUntilKey[] = "until";
constexpr time_t kValidClock = 1704067200; // 2024-01-01 UTC
constexpr char kTag[] = "STAGE1-QUIET";
std::atomic<uint32_t> until_epoch{0};
}

void Init() {
    until_epoch.store(0, std::memory_order_release);
    nvs_handle_t handle;
    const esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return;
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Could not read NVS: %s", esp_err_to_name(err));
        return;
    }
    uint32_t value = 0;
    const esp_err_t read = nvs_get_u32(handle, kUntilKey, &value);
    nvs_close(handle);
    if (read == ESP_OK) until_epoch.store(value, std::memory_order_release);
    else if (read != ESP_ERR_NVS_NOT_FOUND)
        ESP_LOGW(kTag, "Invalid quiet settings: %s", esp_err_to_name(read));
}

bool Active() {
    return RemainingSeconds() != 0;
}

uint32_t RemainingSeconds() {
    const uint32_t until = until_epoch.load(std::memory_order_acquire);
    if (!until) return 0;
    const time_t now = std::time(nullptr);
    if (now < kValidClock || static_cast<uint64_t>(now) >= until) return 0;
    return until - static_cast<uint32_t>(now);
}

bool SetMinutes(int minutes) {
    if (minutes < 0 || minutes > 1440) return false;
    uint32_t next = 0;
    if (minutes) {
        const time_t now = std::time(nullptr);
        if (now < kValidClock || static_cast<uint64_t>(now) >
            std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(minutes) * 60U)
            return false;
        next = static_cast<uint32_t>(now) + static_cast<uint32_t>(minutes) * 60U;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "NVS open failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_u32(handle, kUntilKey, next);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "NVS save failed: %s", esp_err_to_name(err));
        return false;
    }
    until_epoch.store(next, std::memory_order_release);
    stackchan_nvs_commit_observer();
    ESP_LOGI(kTag, "Quiet mode %s, minutes=%d", next ? "enabled" : "disabled", minutes);
    return true;
}
}
