// Web access to the existing SETUP application's settings.
// Never initialize or erase NVS, and never perform reset/OTA actions here.
#include "setup_web.h"
#include <stackchan/config/server_endpoint.h>
#include "wake_net_threshold.h"
#include "conversation_audio_settings.h"
#include "application.h"
#include <atomic>
#include <hal/hal.h>
#include <hal/hal_toggles.h>
#include <apps/common/common.h>
#include <cJSON.h>
#include <esp_log.h>
#include <nvs.h>
#include <sdkconfig.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace stackchan::stage1::setup_web {
namespace {
constexpr const char* kTag = "SETUP-WEB";
constexpr const char* kTimezones[] = {
    "BIT12", "SST11", "HST10", "AKST9", "PST8", "MST7", "CST6",
    "EST5", "AST4", "BRT3", "GST2", "AZOT1", "GMT0", "CET-1",
    "EET-2", "MSK-3", "GST-4", "PKT-5", "BST-6", "ICT-7",
    "CST-8", "JST-9", "AEST-10", "SBT-11", "NZST-12", "FJT-13", "LINT-14"
};
constexpr int kBrightness[] = {1, 15, 30, 45, 60, 75, 90, 100};
constexpr size_t kMaxBody = 384;
// Only the main application task performs the new NVS save and SD notification.
// HTTP requests remain lightweight, using the existing application queue.
enum WakeSaveState { kWakeIdle, kWakePending, kWakeSaved, kWakeFailed };
std::atomic<int> wake_save_state{kWakeIdle};
bool QueueWakeThreshold(int value) {
    if (wake_save_state.exchange(kWakePending) == kWakePending) return false;
    if (!Application::GetInstance().Schedule([value]() {
            const bool saved = WakeNetThreshold::Save(value);
            ESP_LOGI(kTag, "WakeNet threshold NVS save: %d result=%d", value, saved);
            wake_save_state.store(saved ? kWakeSaved : kWakeFailed);
        })) {
        wake_save_state.store(kWakeFailed);
        return false;
    }
    return true;
}
std::atomic<int> conversation_save_state{kWakeIdle};
bool QueueConversationAudio(ConversationAudio::Settings value) {
    if (conversation_save_state.exchange(kWakePending) == kWakePending) return false;
    if (!Application::GetInstance().Schedule([value]() {
            const bool saved = ConversationAudio::Save(value);
            ESP_LOGI(kTag, "Conversation audio NVS save: gain=%d vad=%d agc=%d result=%d",
                     value.gain_db, value.vad_mode, value.agc, saved);
            conversation_save_state.store(saved ? kWakeSaved : kWakeFailed);
        })) {
        conversation_save_state.store(kWakeFailed);
        return false;
    }
    return true;
}
using config::server_endpoint::kMaxServerBase;
using config::server_endpoint::kMaxEndpoint;
using config::server_endpoint::BuildServerEndpoints;
using config::server_endpoint::ServerBaseFromOta;
constexpr const char* kOtaNamespace = "wifi";
constexpr const char* kOtaKey = "ota_url";
constexpr const char* kWebsocketNamespace = "websocket";
constexpr const char* kWebsocketKey = "url";

extern "C" bool stackchan_sd_nvs_backup_pending(void);
extern "C" void stackchan_nvs_commit_observer(void);

esp_err_t Reply(httpd_req_t* req, const char* status, const char* body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}
bool AllowedTimezone(const char* tz) {
    if (!tz) return false;
    for (const auto* allowed : kTimezones)
        if (std::strcmp(tz, allowed) == 0) return true;
    return false;
}
bool Number(const cJSON* value, int min, int max, int step, int& output) {
    if (!cJSON_IsNumber(value) || value->valuedouble != value->valueint ||
        value->valueint < min || value->valueint > max ||
        (value->valueint - min) % step != 0) return false;
    output = value->valueint;
    return true;
}
bool Boolean(const cJSON* value, bool& output) {
    if (!cJSON_IsBool(value)) return false;
    output = cJSON_IsTrue(value);
    return true;
}
bool AllowedBrightness(int value) {
    for (const int item : kBrightness) if (item == value) return true;
    return false;
}
bool MatchAi(const XiaozhiConfig_t& actual, const XiaozhiConfig_t& expected) {
    return actual.idleRandomMovementLevel == expected.idleRandomMovementLevel &&
        actual.startAiAgentOnBoot == expected.startAiAgentOnBoot;
}
void AddBool(cJSON* root, const char* key, bool value) {
    cJSON_AddBoolToObject(root, key, value);
}
bool ReadStoredString(const char* name_space, const char* key,
                      char* output, size_t capacity) {
    if (!output || capacity == 0) return false;
    output[0] = '\0';
    nvs_handle_t handle = 0;
    const esp_err_t opened = nvs_open(name_space, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
    if (opened != ESP_OK) return false;
    size_t length = capacity;
    const esp_err_t read = nvs_get_str(handle, key, output, &length);
    nvs_close(handle);
    if (read == ESP_ERR_NVS_NOT_FOUND) {
        output[0] = '\0';
        return true;
    }
    if (read != ESP_OK) {
        output[0] = '\0';
        return false;
    }
    output[capacity - 1] = '\0';
    return true;
}
bool SaveServerEndpoints(const char* ota, const char* websocket) {
    nvs_handle_t ota_handle = 0;
    nvs_handle_t websocket_handle = 0;
    esp_err_t error = nvs_open(kOtaNamespace, NVS_READWRITE, &ota_handle);
    if (error == ESP_OK)
        error = nvs_open(kWebsocketNamespace, NVS_READWRITE, &websocket_handle);
    if (error == ESP_OK) error = nvs_set_str(ota_handle, kOtaKey, ota);
    if (error == ESP_OK) error = nvs_set_str(websocket_handle, kWebsocketKey, websocket);
    if (error == ESP_OK) error = nvs_commit(ota_handle);
    if (error == ESP_OK) error = nvs_commit(websocket_handle);
    if (websocket_handle) nvs_close(websocket_handle);
    if (ota_handle) nvs_close(ota_handle);
    if (error != ESP_OK) return false;
    stackchan_nvs_commit_observer();
    char confirmed_ota[kMaxEndpoint]{};
    char confirmed_websocket[kMaxEndpoint]{};
    return ReadStoredString(kOtaNamespace, kOtaKey, confirmed_ota, sizeof(confirmed_ota)) &&
           ReadStoredString(kWebsocketNamespace, kWebsocketKey,
                            confirmed_websocket, sizeof(confirmed_websocket)) &&
           std::strcmp(confirmed_ota, ota) == 0 &&
           std::strcmp(confirmed_websocket, websocket) == 0;
}
}  // namespace

esp_err_t Read(httpd_req_t* request) {
    // A tiny status response avoids running the full settings read while
    // the main task is finishing the NVS transaction.
    char query[32]{};
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
        std::strcmp(query, "wake_threshold=1") == 0) {
        const int state = wake_save_state.load();
        char response[96];
        std::snprintf(response, sizeof(response),
                      "{\"state\":%d,\"wake_threshold\":%d}", state, WakeNetThreshold::Get());
        return Reply(request, "200 OK", response);
    }
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
        std::strcmp(query, "conversation_audio=1") == 0) {
        const auto value = ConversationAudio::Get();
        char response[256];
        std::snprintf(response, sizeof(response),
            "{\"state\":%d,\"gain_db\":%d,\"vad_mode\":%d,\"agc\":%d}",
            conversation_save_state.load(), value.gain_db, value.vad_mode, value.agc);
        return Reply(request, "200 OK", response);
    }
    auto* root = cJSON_CreateObject();
    if (!root) return Reply(request, "503 Service Unavailable", "{\"error\":\"low memory\"}");

    auto& hal = GetHAL();
    cJSON_AddNumberToObject(root, "brightness", hal.getBackLightBrightness());
    cJSON_AddNumberToObject(root, "volume", hal.getSpeakerVolume());
    const std::string timezone = hal.getTimezone();
    cJSON_AddStringToObject(root, "timezone", timezone.c_str());

    const auto ai = hal.getXiaozhiConfig();
    cJSON_AddNumberToObject(root, "idle_movement", ai.idleRandomMovementLevel);
    AddBool(root, "start_ai_on_boot", ai.startAiAgentOnBoot);
    cJSON_AddNumberToObject(root, "wake_threshold", WakeNetThreshold::Get());

    char ota_url[kMaxEndpoint]{};
    char server_base[kMaxServerBase + 1]{};
    if (!ReadStoredString(kOtaNamespace, kOtaKey, ota_url, sizeof(ota_url))) {
        cJSON_Delete(root);
        return Reply(request, "500 Internal Server Error", "{\"error\":\"server setting unavailable\"}");
    }
    if (!ota_url[0]) std::snprintf(ota_url, sizeof(ota_url), "%s", CONFIG_OTA_URL);
    if (!ServerBaseFromOta(ota_url, server_base, sizeof(server_base))) server_base[0] = '\0';
    cJSON_AddStringToObject(root, "server_base_url", server_base);

    hal_toggles::ToggleStates states{};
    const bool loaded = hal_toggles::load_states(states);
    AddBool(root, "app_visibility_loaded", loaded);
    if (loaded) {
        unsigned mask = 0;
        for (size_t i = 0; i < hal_toggles::TOGGLE_COUNT; ++i)
            if (states[i]) mask |= 1u << i;
        cJSON_AddNumberToObject(root, "app_visibility_mask", mask);
    }

    // Account information is the existing local cache. Do not start account
    // network requests or disclose authentication tokens from a Web GET.
    const auto account = hal.getUserAccountInfo();
    cJSON_AddStringToObject(root, "account_username", account.username.c_str());
    cJSON_AddStringToObject(root, "device_name", account.deviceName.c_str());
    const std::string version(common::FirmwareVersion);
    cJSON_AddStringToObject(root, "firmware_version", version.c_str());
    const std::string mac = hal.getFactoryMacString(":");
    cJSON_AddStringToObject(root, "mac", mac.c_str());
    cJSON_AddNumberToObject(root, "battery", hal.getBatteryLevel());
    AddBool(root, "charging", hal.isBatteryCharging());
    AddBool(root, "sd_pending", stackchan_sd_nvs_backup_pending());

    // A partial cJSON tree caused by low heap must never look like a valid
    // settings response: otherwise the browser might save missing defaults.
    constexpr const char* required[] = {
        "brightness", "volume", "timezone", "idle_movement", "start_ai_on_boot",
        "server_base_url", "wake_threshold",
        "app_visibility_loaded", "account_username", "device_name",
        "firmware_version", "mac", "battery", "charging", "sd_pending"
    };
    for (const char* key : required) {
        if (!cJSON_GetObjectItemCaseSensitive(root, key)) {
            cJSON_Delete(root);
            return Reply(request, "503 Service Unavailable",
                         "{\"error\":\"low memory\"}");
        }
    }
    if (loaded && !cJSON_GetObjectItemCaseSensitive(root, "app_visibility_mask")) {
        cJSON_Delete(root);
        return Reply(request, "503 Service Unavailable", "{\"error\":\"low memory\"}");
    }
    char* response = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!response) return Reply(request, "503 Service Unavailable", "{\"error\":\"low memory\"}");
    const auto err = Reply(request, "200 OK", response);
    cJSON_free(response);
    return err;
}

esp_err_t Write(httpd_req_t* request) {
    char type[48]{};
    if (httpd_req_get_hdr_value_str(request, "Content-Type", type, sizeof(type)) != ESP_OK ||
        std::strncmp(type, "application/json", 16) != 0)
        return Reply(request, "415 Unsupported Media Type", "{\"error\":\"JSON required\"}");
    if (request->content_len <= 0 ||
        static_cast<size_t>(request->content_len) > kMaxBody)
        return Reply(request, "413 Payload Too Large", "{\"error\":\"invalid size\"}");
    char body[kMaxBody + 1]{};
    size_t received = 0;
    while (received < static_cast<size_t>(request->content_len)) {
        const int n = httpd_req_recv(request, body + received,
                                    request->content_len - received);
        if (n <= 0)
            return Reply(request, "400 Bad Request", "{\"error\":\"incomplete request\"}");
        received += static_cast<size_t>(n);
    }

    cJSON* root = cJSON_ParseWithLength(body, received);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return Reply(request, "400 Bad Request", "{\"error\":\"invalid JSON\"}");
    }
    const cJSON* field_json = cJSON_GetObjectItemCaseSensitive(root, "field");
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(root, "value");
    const char* field = cJSON_IsString(field_json) ? field_json->valuestring : nullptr;
    bool valid = field && value;
    bool saved = false;
    bool needs_restart = false;
    if (valid && std::strcmp(field, "brightness") == 0) {
        int brightness = 0;
        valid = Number(value, 1, 100, 1, brightness) && AllowedBrightness(brightness);
        if (valid) {
            GetHAL().setBackLightBrightness(static_cast<uint8_t>(brightness), true);
            saved = GetHAL().getBackLightBrightness() == brightness;
        }
    } else if (valid && std::strcmp(field, "volume") == 0) {
        int volume = 0;
        valid = Number(value, 0, 100, 5, volume);
        if (valid) {
            GetHAL().setSpeakerVolume(static_cast<uint8_t>(volume), true);
            saved = GetHAL().getSpeakerVolume() == volume;
        }
    } else if (valid && std::strcmp(field, "timezone") == 0) {
        const char* timezone = cJSON_IsString(value) ? value->valuestring : nullptr;
        valid = AllowedTimezone(timezone);
        if (valid) {
            GetHAL().setTimezone(timezone);
            saved = GetHAL().getTimezone() == timezone;
        }
    } else if (valid &&
        (std::strcmp(field, "idle_movement") == 0 ||
         std::strcmp(field, "start_ai_on_boot") == 0)) {
        auto ai = GetHAL().getXiaozhiConfig();
        if (std::strcmp(field, "idle_movement") == 0) {
            int level = 0;
            valid = Number(value, 0, 3, 1, level);
            if (valid) ai.idleRandomMovementLevel = static_cast<uint8_t>(level);
        } else {
            bool enabled = false;
            valid = Boolean(value, enabled);
            if (valid) ai.startAiAgentOnBoot = enabled;
        }
        if (valid) {
            GetHAL().setXiaozhiConfig(ai);
            saved = MatchAi(GetHAL().getXiaozhiConfig(), ai);
            needs_restart = true; // Matches SETUP application's warm-reboot semantics.
        }
    } else if (valid && std::strcmp(field, "wake_threshold") == 0) {
        int threshold = 0;
        valid = Number(value, WakeNetThreshold::kDefault, WakeNetThreshold::kMax, 1, threshold) &&
                WakeNetThreshold::IsValid(threshold);
        cJSON_Delete(root);
        if (!valid) return Reply(request, "400 Bad Request", "{\"error\":\"invalid threshold\"}");
        if (!QueueWakeThreshold(threshold))
            return Reply(request, "503 Service Unavailable", "{\"error\":\"save busy or queue unavailable\"}");
        return Reply(request, "202 Accepted", "{\"queued\":true}");
    } else if (valid && std::strcmp(field, "conversation_audio") == 0) {
        ConversationAudio::Settings settings;
        valid = cJSON_IsObject(value) &&
            Number(cJSON_GetObjectItemCaseSensitive(value, "gain_db"),
                   ConversationAudio::kGainMin, ConversationAudio::kGainMax, 1, settings.gain_db) &&
            Number(cJSON_GetObjectItemCaseSensitive(value, "vad_mode"),
                   0, 2, 1, settings.vad_mode) &&
            Number(cJSON_GetObjectItemCaseSensitive(value, "agc"),
                   0, ConversationAudio::kAgcMax, 1, settings.agc);
        cJSON_Delete(root);
        if (!valid) return Reply(request, "400 Bad Request", "{\"error\":\"invalid conversation audio settings\"}");
        if (!QueueConversationAudio(settings))
            return Reply(request, "503 Service Unavailable", "{\"error\":\"save busy or queue unavailable\"}");
        return Reply(request, "202 Accepted", "{\"queued\":true}");
    } else if (valid && std::strcmp(field, "server_base_url") == 0) {
        const char* base = cJSON_IsString(value) ? value->valuestring : nullptr;
        char ota_url[kMaxEndpoint]{};
        char websocket_url[kMaxEndpoint]{};
        valid = BuildServerEndpoints(base, ota_url, sizeof(ota_url),
                                     websocket_url, sizeof(websocket_url));
        if (valid) {
            saved = SaveServerEndpoints(ota_url, websocket_url);
            needs_restart = true;
        }
    } else if (valid && std::strcmp(field, "app_visibility") == 0) {
        const cJSON* app_json = cJSON_GetObjectItemCaseSensitive(root, "app");
        const char* app = cJSON_IsString(app_json) ? app_json->valuestring : nullptr;
        bool enabled = false;
        valid = app && Boolean(value, enabled);
        int index = -1;
        if (valid) {
            for (size_t i = 0; i < hal_toggles::TOGGLE_COUNT; ++i) {
                if (std::strcmp(app, hal_toggles::TOGGLE_LIST[i].key) == 0) {
                    index = static_cast<int>(i);
                    break;
                }
            }
            valid = index >= 0;
        }
        if (valid) {
            hal_toggles::ToggleStates states{};
            if (!hal_toggles::load_states(states)) {
                cJSON_Delete(root);
                return Reply(request, "503 Service Unavailable",
                             "{\"error\":\"app preferences unavailable\"}");
            }
            states[index] = enabled;
            if (hal_toggles::save_states(states)) {
                hal_toggles::ToggleStates confirmed{};
                saved = hal_toggles::load_states(confirmed) && confirmed == states;
            }
        }
    } else {
        valid = false;
    }

    if (!saved)
        ESP_LOGW(kTag, "SETUP Web save failed: field=%s valid=%d",
                 field ? field : "-", valid);
    cJSON_Delete(root);
    if (!valid) return Reply(request, "400 Bad Request", "{\"error\":\"invalid field or value\"}");
    if (!saved) return Reply(request, "500 Internal Server Error", "{\"error\":\"setting not confirmed\"}");
    // NVS commits are already observed by the existing asynchronous SD
    // snapshot worker. Never claim that an SD backup has finished here.
    return Reply(request, "200 OK", needs_restart ?
        "{\"saved\":true,\"sd\":\"queued\",\"reboot_required\":true}" :
        "{\"saved\":true,\"sd\":\"queued\",\"reboot_required\":false}");
}
}  // namespace stackchan::stage1::setup_web
