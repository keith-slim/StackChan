#include "ip_store.h"
#include <nvs.h>
#include <esp_log.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace stackchan::wifi {
namespace {
constexpr char kNamespace[] = "wifi_ipv4";
constexpr char kTag[] = "StackChanWiFi";
constexpr uint8_t kFormatVersion = 1;
struct StoredIp {
    uint8_t version;
    char ssid[33];
    IpSettings config;
};

bool KeyFor(std::string_view ssid, char (&key)[16]) {
    if (ssid.empty() || ssid.size() > 32 || ssid.find('\0') != std::string_view::npos)
        return false;
    // 56-bit FNV-1a plus the full SSID in the blob to reject hash collisions.
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned char c : ssid) {
        hash ^= c;
        hash *= UINT64_C(1099511628211);
    }
    return std::snprintf(key, sizeof(key), "i%014llx",
                         static_cast<unsigned long long>(hash & UINT64_C(0x00ffffffffffffff))) == 15;
}

bool UpdateIp(std::string_view ssid, const IpSettings* config) {
    char key[16]{};
    if (!KeyFor(ssid, key) || (config && !ValidIpSettings(*config))) return false;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "IPv4 NVS open failed: %s", esp_err_to_name(err));
        return false;
    }
    if (!config || !config->use_static) {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    } else {
        StoredIp stored{};
        stored.version = kFormatVersion;
        std::memcpy(stored.ssid, ssid.data(), ssid.size());
        stored.config = *config;
        err = nvs_set_blob(h, key, &stored, sizeof(stored));
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGE(kTag, "IPv4 NVS save failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}
} // namespace

bool LoadIpSettings(std::string_view ssid, IpSettings& out) {
    out = {};
    char key[16]{};
    if (!KeyFor(ssid, key)) return false;
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(kNamespace, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    StoredIp stored{};
    size_t length = sizeof(stored);
    err = nvs_get_blob(h, key, &stored, &length);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK || length != sizeof(stored) ||
        stored.version != kFormatVersion ||
        std::strlen(stored.ssid) != ssid.size() ||
        std::memcmp(stored.ssid, ssid.data(), ssid.size()) != 0 ||
        !ValidIpSettings(stored.config)) {
        ESP_LOGW(kTag, "Invalid saved IPv4 settings for the requested SSID");
        return false;
    }
    out = stored.config;
    return true;
}

bool SaveIpSettings(std::string_view ssid, const IpSettings& settings) {
    return UpdateIp(ssid, &settings);
}

bool RemoveIpSettings(std::string_view ssid) {
    return UpdateIp(ssid, nullptr);
}
} // namespace stackchan::wifi
