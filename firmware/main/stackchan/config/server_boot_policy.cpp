#include "server_boot_policy.h"
#include "server_endpoint.h"
#include <nvs.h>
#include <esp_log.h>
#include <cstring>

extern "C" void stackchan_nvs_commit_observer(void);

namespace stackchan::config::server_boot {
namespace {
using namespace server_endpoint;
constexpr const char* TAG = "SC_BOOT_CONFIG";

bool ReadString(const char* ns, const char* key, char* out, size_t capacity) {
    out[0] = '\0';
    nvs_handle_t h = 0;
    const esp_err_t opened = nvs_open(ns, NVS_READONLY, &h);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
    if (opened != ESP_OK) return false;
    const esp_err_t err = nvs_get_str(h, key, out, &capacity);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) { out[0] = '\0'; return true; }
    return err == ESP_OK;
}
bool ReadVersion(int32_t& version) {
    version = 0;
    nvs_handle_t h = 0;
    const esp_err_t opened = nvs_open("websocket", NVS_READONLY, &h);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
    if (opened != ESP_OK) return false;
    const esp_err_t err = nvs_get_i32(h, "version", &version);
    nvs_close(h);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND;
}
Plan ReadPlan(char* ws, size_t capacity, int32_t& version) {
    char mqtt[kMaxEndpoint]{};
    if (!ReadString("websocket", "url", ws, capacity) ||
        !ReadString("mqtt", "endpoint", mqtt, sizeof mqtt)) return {};
    if (mqtt[0]) return PlanBoot(true, ws[0] != 0, false, true, false, 0);
    if (!ReadVersion(version)) return {};
    char base[kMaxServerBase + 1]{};
    if (ws[0]) {
        return PlanBoot(true, true, ServerBaseFromWebsocket(ws, base, sizeof base),
                        false, false, version);
    }
    char legacy[kMaxEndpoint]{};
    char unused_ota[kMaxEndpoint]{};
    const bool convertible = ReadString("wifi", "ota_url", legacy, sizeof legacy) &&
        ServerBaseFromOta(legacy, base, sizeof base) &&
        BuildServerEndpoints(base, unused_ota, sizeof unused_ota, ws, capacity);
    return PlanBoot(true, false, false, false, convertible, version);
}
} // namespace

void PrepareStoredProtocol() {
    char ws[kMaxEndpoint]{};
    int32_t version = 0;
    const auto plan = ReadPlan(ws, sizeof ws, version);
    if (plan.protocol != Protocol::Websocket || (!plan.seed_url && !plan.seed_version)) return;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open("websocket", NVS_READWRITE, &h);
    // Version first: a power loss cannot publish a new URL before its
    // required protocol version. Existing explicit versions and tokens stay.
    if (err == ESP_OK && plan.seed_version) err = nvs_set_i32(h, "version", 3);
    if (err == ESP_OK && plan.seed_url) err = nvs_set_str(h, "url", ws);
    if (err == ESP_OK) err = nvs_commit(h);
    if (h) nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Stored connection migration failed: %s", esp_err_to_name(err));
        return;
    }
    char confirmed[kMaxEndpoint]{};
    int32_t confirmed_version = 0;
    const auto result = ReadPlan(confirmed, sizeof confirmed, confirmed_version);
    if (result.protocol != Protocol::Websocket || result.seed_url || result.seed_version ||
        std::strcmp(ws, confirmed) != 0 ||
        confirmed_version != (plan.seed_version ? 3 : version)) {
        ESP_LOGW(TAG, "Stored connection migration readback failed");
        return;
    }
    stackchan_nvs_commit_observer();
    ESP_LOGI(TAG, "Stored WebSocket configuration migrated (url=%d version=%d)",
             plan.seed_url, plan.seed_version);
}

Protocol ReadDirectProtocol() {
    char ws[kMaxEndpoint]{};
    int32_t version = 0;
    const auto plan = ReadPlan(ws, sizeof ws, version);
    return plan.seed_url || plan.seed_version ? Protocol::Legacy : plan.protocol;
}
} // namespace stackchan::config::server_boot
