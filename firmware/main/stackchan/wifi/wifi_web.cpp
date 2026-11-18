#include "wifi_web.h"
#include "ip_store.h"

#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <wifi_manager.h>
#include <ssid_manager.h>
#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace stackchan::wifi::web {
namespace {
constexpr char kTag[] = "StackChanWiFiWeb";
constexpr int kMaxSavedSsid = 10;
constexpr size_t kMaxBody = 768;
std::atomic<uint32_t> reconnect_after_ms{0};

extern "C" void stackchan_nvs_commit_observer(void);
extern "C" bool stackchan_sd_nvs_backup_pending(void);

esp_err_t Reply(httpd_req_t* req, const char* code, const char* body) {
    httpd_resp_set_status(req, code);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

bool ValidSsid(const std::string& s) {
    return !s.empty() && s.size() <= 32 && s.find('\0') == std::string::npos;
}

const char* StringValue(const cJSON* obj, const char* key) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(value) ? value->valuestring : nullptr;
}

bool ReadStaticValue(const cJSON* obj, const char* key, uint8_t (&result)[4],
                     bool required) {
    const char* value = StringValue(obj, key);
    if (!value) return !required &&
        !cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!*value) return !required;
    return ParseIPv4(value, result);
}

bool ReadIpConfig(const cJSON* obj, IpSettings& out) {
    out = {};
    const char* mode = StringValue(obj, "mode");
    if (!mode) return false;
    if (!std::strcmp(mode, "dhcp")) return true;
    if (std::strcmp(mode, "static")) return false;
    out.use_static = 1;
    return ReadStaticValue(obj, "ip", out.ip, true) &&
           ReadStaticValue(obj, "subnet", out.subnet, true) &&
           ReadStaticValue(obj, "gw", out.gateway, true) &&
           ReadStaticValue(obj, "dns1", out.dns1, true) &&
           ReadStaticValue(obj, "dns2", out.dns2, false) &&
           ValidIpSettings(out);
}

void AddIPv4(cJSON* obj, const char* key, const uint8_t (&ip)[4]) {
    char text[16]{};
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    cJSON_AddStringToObject(obj, key, text);
}

// Check SsidManager's existing NVS file format, not just its in-memory list.
bool NvsCredentialsMatch(const std::string& ssid, const std::string& password) {
    nvs_handle_t h = 0;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return false;
    bool found = false;
    for (int i = 0; i < kMaxSavedSsid; ++i) {
        const std::string suffix = i ? std::to_string(i) : "";
        const std::string ssid_key = "ssid" + suffix;
        const std::string pass_key = "password" + suffix;
        char stored_ssid[33]{}, stored_password[65]{};
        size_t size = sizeof(stored_ssid);
        if (nvs_get_str(h, ssid_key.c_str(), stored_ssid, &size) != ESP_OK ||
            ssid != stored_ssid) continue;
        size = sizeof(stored_password);
        found = nvs_get_str(h, pass_key.c_str(), stored_password, &size) == ESP_OK &&
                password == stored_password;
        break;
    }
    nvs_close(h);
    return found;
}

bool NvsSsidAbsent(const std::string& ssid) {
    nvs_handle_t h = 0;
    const esp_err_t open = nvs_open("wifi", NVS_READONLY, &h);
    if (open == ESP_ERR_NVS_NOT_FOUND) return false;
    if (open != ESP_OK) return false;
    bool found = false;
    for (int i = 0; i < kMaxSavedSsid; ++i) {
        const std::string suffix = i ? std::to_string(i) : "";
        const std::string key = "ssid" + suffix;
        char value[33]{};
        size_t length = sizeof(value);
        const esp_err_t read = nvs_get_str(h, key.c_str(), value, &length);
        if (read != ESP_OK && read != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(h);
            return false;
        }
        if (read == ESP_OK && ssid == value) {
            found = true;
            break;
        }
    }
    nvs_close(h);
    return !found;
}

void RequestReconnect() {
    reconnect_after_ms.store(GetHAL().millis() + 1200, std::memory_order_release);
}

bool Busy() {
    return hal_bridge::is_xiaozhi_ready() && !hal_bridge::is_xiaozhi_idle();
}
} // namespace

esp_err_t ReadSettings(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON* items = cJSON_CreateArray();
    if (!root || !items) {
        cJSON_Delete(root); cJSON_Delete(items);
        return Reply(req, "503 Service Unavailable", "{\"error\":\"low memory\"}");
    }
    cJSON_AddItemToObject(root, "networks", items);
    auto& manager = SsidManager::GetInstance();
    for (const auto& entry : manager.GetSsidList()) {
        cJSON* row = cJSON_CreateObject();
        if (!row) { cJSON_Delete(root); return Reply(req,"503 Service Unavailable","{\"error\":\"low memory\"}"); }
        cJSON_AddStringToObject(row, "ssid", entry.ssid.c_str());
        cJSON_AddBoolToObject(row, "password_set", !entry.password.empty());
        IpSettings settings{};
        const bool loaded = LoadIpSettings(entry.ssid, settings);
        cJSON_AddBoolToObject(row, "ip_valid", loaded);
        cJSON_AddStringToObject(row, "mode", settings.use_static ? "static" : "dhcp");
        if (loaded && settings.use_static) {
            AddIPv4(row, "ip", settings.ip);
            AddIPv4(row, "subnet", settings.subnet);
            AddIPv4(row, "gw", settings.gateway);
            AddIPv4(row, "dns1", settings.dns1);
            if (IPv4Number(settings.dns2)) AddIPv4(row, "dns2", settings.dns2);
        }
        cJSON_AddItemToArray(items, row);
    }
    auto& wifi = WifiManager::GetInstance();
    cJSON_AddBoolToObject(root, "connected", wifi.IsConnected());
    cJSON_AddStringToObject(root, "connected_ssid", wifi.GetSsid().c_str());
    cJSON_AddStringToObject(root, "connected_ip", wifi.GetIpAddress().c_str());
    cJSON_AddBoolToObject(root, "reconnect_pending",
                          reconnect_after_ms.load(std::memory_order_acquire) != 0);
    cJSON_AddBoolToObject(root, "sd_pending", stackchan_sd_nvs_backup_pending());
    char* serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!serialized) return Reply(req, "503 Service Unavailable", "{\"error\":\"low memory\"}");
    const esp_err_t result = Reply(req, "200 OK", serialized);
    cJSON_free(serialized);
    return result;
}

esp_err_t WriteSettings(httpd_req_t* req) {
    if (Busy()) return Reply(req,"409 Conflict","{\"error\":\"conversation busy\"}");
    char type[48]{};
    if (httpd_req_get_hdr_value_str(req,"Content-Type",type,sizeof(type)) != ESP_OK ||
        std::strncmp(type,"application/json",16) != 0)
        return Reply(req,"415 Unsupported Media Type","{\"error\":\"JSON required\"}");
    if (req->content_len <= 0 || static_cast<size_t>(req->content_len) > kMaxBody)
        return Reply(req,"413 Payload Too Large","{\"error\":\"invalid size\"}");
    char input[kMaxBody + 1]{};
    size_t received = 0;
    while (received < static_cast<size_t>(req->content_len)) {
        const int size = httpd_req_recv(req, input + received,
                                       req->content_len - received);
        if (size <= 0) return Reply(req,"400 Bad Request","{\"error\":\"incomplete payload\"}");
        received += static_cast<size_t>(size);
    }
    cJSON* obj = cJSON_ParseWithLength(input, received);
    if (!cJSON_IsObject(obj)) {
        cJSON_Delete(obj);
        return Reply(req,"400 Bad Request","{\"error\":\"invalid JSON\"}");
    }
    const char* action = StringValue(obj, "action");
    const char* ssid_text = StringValue(obj, "ssid");
    const std::string ssid = ssid_text ? ssid_text : "";
    if (!action || !ValidSsid(ssid)) {
        cJSON_Delete(obj);
        return Reply(req,"400 Bad Request","{\"error\":\"invalid action or SSID\"}");
    }
    auto& manager = SsidManager::GetInstance();
    const auto& list = manager.GetSsidList();
    int index = -1;
    for (size_t i=0; i<list.size(); ++i)
        if (list[i].ssid == ssid) { index = static_cast<int>(i); break; }

    if (std::strcmp(action,"delete") == 0) {
        if (index < 0 || list.size() <= 1 || WifiManager::GetInstance().GetSsid() == ssid) {
            cJSON_Delete(obj);
            return Reply(req,"409 Conflict","{\"error\":\"cannot remove active or last SSID\"}");
        }
        manager.RemoveSsid(index);
        if (!NvsSsidAbsent(ssid)) {
            cJSON_Delete(obj);
            return Reply(req,"500 Internal Server Error","{\"error\":\"SSID NVS delete failed\"}");
        }
        if (!RemoveIpSettings(ssid)) {
            cJSON_Delete(obj);
            return Reply(req,"500 Internal Server Error","{\"error\":\"IPv4 NVS delete failed\"}");
        }
        stackchan_nvs_commit_observer();
        cJSON_Delete(obj);
        return Reply(req,"200 OK","{\"saved_nvs\":true,\"sd\":\"queued\",\"reconnect\":false}");
    }
    if (std::strcmp(action,"save") != 0 || (index < 0 && list.size() >= kMaxSavedSsid)) {
        cJSON_Delete(obj);
        return Reply(req,"400 Bad Request","{\"error\":\"invalid action or SSID list full\"}");
    }
    IpSettings ip{};
    if (!ReadIpConfig(obj, ip)) {
        cJSON_Delete(obj);
        return Reply(req,"400 Bad Request","{\"error\":\"invalid IPv4 configuration\"}");
    }
    const char* password_text = StringValue(obj, "password");
    const cJSON* clear = cJSON_GetObjectItemCaseSensitive(obj,"clear_password");
    const bool clear_password = cJSON_IsTrue(clear);
    std::string password = index >= 0 ? list[index].password : "";
    if (clear_password) password.clear();
    else if (password_text && *password_text) password = password_text;
    const bool credentials_changed = index < 0 || password != list[index].password;
    // Keep existing 64-hex WPA2 keys intact. A newly supplied 64-character
    // value is allowed only when all characters are hexadecimal.
    if (password.size() > 64 ||
        (credentials_changed && password.size() == 64 &&
         password.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)) {
        cJSON_Delete(obj);
        return Reply(req,"400 Bad Request","{\"error\":\"invalid password length\"}");
    }

    IpSettings previous{};
    if (!LoadIpSettings(ssid, previous)) {
        // Overwriting a corrupt IPv4 blob is a safe recovery operation.
        // The connection path already falls back to DHCP for invalid data.
        ESP_LOGW(kTag, "Replacing invalid saved IPv4 settings");
        previous = {};
    }
    if (!SaveIpSettings(ssid, ip)) {
        cJSON_Delete(obj);
        return Reply(req,"500 Internal Server Error","{\"error\":\"IPv4 NVS save failed\"}");
    }
    if (credentials_changed) {
        manager.AddSsid(ssid, password);
        if (!NvsCredentialsMatch(ssid, password)) {
            SaveIpSettings(ssid, previous);
            cJSON_Delete(obj);
            return Reply(req,"500 Internal Server Error","{\"error\":\"SSID NVS save failed\"}");
        }
    }
    stackchan_nvs_commit_observer();
    const cJSON* connect = cJSON_GetObjectItemCaseSensitive(obj, "reconnect");
    const bool reconnect = cJSON_IsTrue(connect) ||
                           WifiManager::GetInstance().GetSsid() == ssid;
    if (reconnect) RequestReconnect();
    cJSON_Delete(obj);
    return Reply(req,"200 OK",reconnect ?
        "{\"saved_nvs\":true,\"sd\":\"queued\",\"reconnect\":true}" :
        "{\"saved_nvs\":true,\"sd\":\"queued\",\"reconnect\":false}");
}

void ApplyPendingIfIdle() {
    uint32_t ready_at = reconnect_after_ms.load(std::memory_order_acquire);
    if (!ready_at || static_cast<int32_t>(GetHAL().millis()-ready_at) < 0 || Busy()) return;
    if (!reconnect_after_ms.compare_exchange_strong(
            ready_at, 0, std::memory_order_acq_rel)) return;
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized()) return;
    ESP_LOGI(kTag,"Applying saved Wi-Fi settings without reboot");
    wifi.StopStation();
    wifi.StartStation();
}
} // namespace stackchan::wifi::web
