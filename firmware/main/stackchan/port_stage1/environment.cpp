#include "environment.h"

#include <ArduinoJson.h>
#include <board.h>
#include <http.h>
#include <network_interface.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include <nvs.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <ctime>
#include <mbedtls/base64.h>

extern "C" void stackchan_nvs_commit_observer(void);

namespace stackchan::stage1::environment {
namespace {

constexpr const char* TAG = "SC_ENV";
constexpr const char* NVS_NAMESPACE = "sc_weather";
constexpr uint64_t LOCATION_INTERVAL_MS = 60ULL * 60ULL * 1000ULL;
constexpr uint64_t WEATHER_INTERVAL_MS = 10ULL * 60ULL * 1000ULL;
constexpr uint64_t WEATHER_STALE_MS = 30ULL * 60ULL * 1000ULL;
constexpr uint64_t RETRY_INTERVAL_MS = 60ULL * 1000ULL;
constexpr uint64_t IDLE_RECHECK_MS = 5ULL * 1000ULL;
constexpr int HTTP_TIMEOUT_MS = 3000;

struct LocationInfo {
    char city[128] = {};
    char region[128] = {};
    double latitude = 0;
    double longitude = 0;
};

struct WeatherInfo {
    double temperature_c = 0;
    unsigned humidity_percent = 0;
    unsigned condition_code = 0;
    char description[128] = {};
    char icon[4] = {};
    int32_t utc_offset_seconds = 0;
};

// The cache and API key live in PSRAM. Internal RAM keeps only synchronization
// primitives and the small FreeRTOS task-control block.
struct State {
    Settings settings;
    LocationInfo location;
    WeatherInfo weather;
    bool location_valid = false;
    bool weather_valid = false;
    bool worker_active = false;
    bool have_network = false;
    uint8_t bssid[6] = {};
    uint64_t location_at = 0;
    uint64_t weather_at = 0;
    uint64_t next_poll = 0;
    uint32_t revision = 0;
    int location_http = 0;
    int weather_http = 0;
};

State* state = nullptr;
std::mutex state_mutex;
std::atomic<bool> idle_allowed{false};

uint64_t NowMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}

class PsramJsonAllocator final : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    void deallocate(void* pointer) override { heap_caps_free(pointer); }
    void* reallocate(void* pointer, size_t size) override {
        return heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
};

bool CopyText(const char* source, char* output, size_t capacity) {
    if (!source || !output || capacity == 0) return false;
    const size_t length = std::strlen(source);
    if (length == 0 || length >= capacity) return false;
    std::memcpy(output, source, length + 1);
    return true;
}

bool ReadUrl(const char* url, char* body, size_t capacity, int& status_code) {
    status_code = 0;
    if (!url || !body || capacity < 2) return false;
    auto* network = Board::GetInstance().GetNetwork();
    if (!network) return false;
    auto http = network->CreateHttp();
    if (!http) return false;
    http->SetTimeout(HTTP_TIMEOUT_MS);
    http->SetKeepAlive(false);
    auto opened = http->Open("GET", url);
    if (!opened) {
        http->Close();
        return false;
    }
    auto response = http->GetStatusCode();
    if (!response) {
        http->Close();
        return false;
    }
    status_code = *response;
    if (status_code != 200) {
        http->Close();
        return false;
    }
    size_t used = 0;
    while (used + 1 < capacity) {
        auto read = http->Read(body + used, capacity - used - 1);
        if (!read) {
            http->Close();
            return false;
        }
        if (*read < 0) {
            http->Close();
            return false;
        }
        if (*read == 0) break;
        used += static_cast<size_t>(*read);
    }
    bool complete = used + 1 < capacity;
    if (!complete) {
        char extra = 0;
        auto read = http->Read(&extra, 1);
        complete = read && *read == 0;
    }
    http->Close();
    body[used] = '\0';
    return complete && used != 0;
}

bool FetchLocation(LocationInfo& output, int& status_code) {
    constexpr char URL[] =
        "http://ip-api.com/json/?fields=status,message,regionName,city,lat,lon&lang=ja";
    using Buffer = std::unique_ptr<char, decltype(&heap_caps_free)>;
    Buffer body(static_cast<char*>(heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                heap_caps_free);
    if (!body || !ReadUrl(URL, body.get(), 1024, status_code)) return false;
    PsramJsonAllocator allocator;
    ArduinoJson::JsonDocument document(&allocator);
    if (ArduinoJson::deserializeJson(document, body.get())) return false;
    const auto root = document.as<ArduinoJson::JsonObjectConst>();
    const char* result = root["status"] | "";
    const char* city = root["city"] | "";
    const char* region = root["regionName"] | "";
    if (std::strcmp(result, "success") != 0 || !root["lat"].is<double>() ||
        !root["lon"].is<double>()) return false;
    LocationInfo updated;
    updated.latitude = root["lat"].as<double>();
    updated.longitude = root["lon"].as<double>();
    if (!std::isfinite(updated.latitude) || !std::isfinite(updated.longitude) ||
        updated.latitude < -90 || updated.latitude > 90 ||
        updated.longitude < -180 || updated.longitude > 180 ||
        !CopyText(city, updated.city, sizeof(updated.city))) return false;
    if (region && region[0] && !CopyText(region, updated.region, sizeof(updated.region))) return false;
    output = updated;
    return true;
}

bool FetchWeather(const LocationInfo& location, const char* api_key,
                  WeatherInfo& output, int& status_code) {
    if (!api_key || !api_key[0]) return false;
    char url[384];
    const int length = std::snprintf(
        url, sizeof(url),
        "https://api.openweathermap.org/data/2.5/weather?lat=%.6f&lon=%.6f&appid=%s&units=metric&lang=ja",
        location.latitude, location.longitude, api_key);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(url)) return false;
    using Buffer = std::unique_ptr<char, decltype(&heap_caps_free)>;
    Buffer body(static_cast<char*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                heap_caps_free);
    const bool read = body && ReadUrl(url, body.get(), 4096, status_code);
    std::memset(url, 0, sizeof(url));
    if (!read) return false;

    PsramJsonAllocator allocator;
    ArduinoJson::JsonDocument document(&allocator);
    if (ArduinoJson::deserializeJson(document, body.get())) return false;
    const auto root = document.as<ArduinoJson::JsonObjectConst>();
    const auto main = root["main"].as<ArduinoJson::JsonObjectConst>();
    const auto conditions = root["weather"].as<ArduinoJson::JsonArrayConst>();
    if (main.isNull() || conditions.isNull() || conditions.size() == 0) return false;
    const auto condition = conditions[0].as<ArduinoJson::JsonObjectConst>();
    if (!main["temp"].is<double>() || !main["humidity"].is<double>() ||
        !condition["id"].is<double>() || !root["timezone"].is<double>()) return false;
    const double temperature = main["temp"].as<double>();
    const double humidity = main["humidity"].as<double>();
    const double code = condition["id"].as<double>();
    const double offset = root["timezone"].as<double>();
    const char* description = condition["description"] | "";
    const char* icon = condition["icon"] | "";
    if (!std::isfinite(temperature) || humidity < 0 || humidity > 100 ||
        std::floor(humidity) != humidity || code < 0 || code > 999 ||
        std::floor(code) != code || offset < -86400 || offset > 86400 ||
        std::floor(offset) != offset || std::strlen(icon) != 3 ||
        icon[0] < '0' || icon[0] > '9' || icon[1] < '0' || icon[1] > '9' ||
        (icon[2] != 'd' && icon[2] != 'n')) return false;
    WeatherInfo updated;
    updated.temperature_c = temperature;
    updated.humidity_percent = static_cast<unsigned>(humidity);
    updated.condition_code = static_cast<unsigned>(code);
    updated.utc_offset_seconds = static_cast<int32_t>(offset);
    if (!CopyText(description, updated.description, sizeof(updated.description)) ||
        !CopyText(icon, updated.icon, sizeof(updated.icon))) return false;
    output = updated;
    return true;
}

uint64_t NextDueLocked(uint64_t now) {
    uint64_t due = state->location_valid ? state->location_at + LOCATION_INTERVAL_MS
                                         : now + RETRY_INTERVAL_MS;
    if (state->settings.weather_api_key[0]) {
        const uint64_t weather_due = state->weather_valid
            ? state->weather_at + WEATHER_INTERVAL_MS : now + RETRY_INTERVAL_MS;
        due = std::min(due, weather_due);
    }
    return std::max(due, now + IDLE_RECHECK_MS);
}

void Worker(void*) {
    Settings settings;
    LocationInfo location;
    uint32_t revision = 0;
    bool need_location = false;
    bool need_weather = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        const uint64_t now = NowMs();
        settings = state->settings;
        location = state->location;
        revision = state->revision;
        need_location = !state->location_valid || now - state->location_at >= LOCATION_INTERVAL_MS;
        need_weather = settings.weather_api_key[0] &&
                       (!state->weather_valid || now - state->weather_at >= WEATHER_INTERVAL_MS);
    }

    int location_http = 0;
    int weather_http = 0;
    bool got_location = false;
    bool got_weather = false;
    bool weather_deferred = false;
    WeatherInfo weather;
    if (idle_allowed.load() && need_location)
        got_location = FetchLocation(location, location_http);
    if (idle_allowed.load() && settings.weather_api_key[0] &&
        (need_weather || got_location) && (!need_location || got_location)) {
        got_weather = FetchWeather(location, settings.weather_api_key, weather, weather_http);
    } else if (settings.weather_api_key[0] && (need_weather || got_location)) {
        weather_deferred = true;
    }
    std::memset(settings.weather_api_key, 0, sizeof(settings.weather_api_key));

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        state->worker_active = false;
        if (revision == state->revision) {
            const uint64_t now = NowMs();
            if (need_location) state->location_http = location_http;
            if (need_weather || got_location) state->weather_http = weather_http;
            if (got_location) {
                if (state->location_valid &&
                    (state->location.latitude != location.latitude ||
                     state->location.longitude != location.longitude))
                    state->weather_valid = false;
                state->location = location;
                state->location_valid = true;
                state->location_at = now;
            }
            if (got_weather) {
                state->weather = weather;
                state->weather_valid = true;
                state->weather_at = now;
            }
            const bool failed = (need_location && !got_location) ||
                                (need_weather && !got_weather && !weather_deferred);
            state->next_poll = failed ? now + RETRY_INTERVAL_MS :
                               weather_deferred ? now + IDLE_RECHECK_MS : NextDueLocked(now);
        }
    }
    ESP_LOGI(TAG, "fetch complete location=%d http=%d weather=%d http=%d",
             got_location, location_http, got_weather, weather_http);
    vTaskDeleteWithCaps(nullptr);
}

bool LoadSettings(Settings& output) {
    nvs_handle_t handle;
    const esp_err_t opened = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (opened == ESP_ERR_NVS_NOT_FOUND) return true;
    if (opened != ESP_OK) return false;
    size_t key_size = sizeof(output.weather_api_key);
    size_t region_size = sizeof(output.region_name);
    const esp_err_t key = nvs_get_str(handle, "api_key", output.weather_api_key, &key_size);
    const esp_err_t region = nvs_get_str(handle, "region", output.region_name, &region_size);
    nvs_close(handle);
    return (key == ESP_OK || key == ESP_ERR_NVS_NOT_FOUND) &&
           (region == ESP_OK || region == ESP_ERR_NVS_NOT_FOUND);
}

} // namespace

bool ValidSettings(const Settings& value) {
    const size_t key_length = strnlen(value.weather_api_key, sizeof(value.weather_api_key));
    const size_t region_length = strnlen(value.region_name, sizeof(value.region_name));
    if (key_length >= sizeof(value.weather_api_key) ||
        region_length >= sizeof(value.region_name)) return false;
    for (size_t i = 0; i < key_length; ++i) {
        const char c = value.weather_api_key[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    for (size_t i = 0; i < region_length; ++i)
        if (static_cast<unsigned char>(value.region_name[i]) < 0x20) return false;
    return true;
}

bool Init() {
    std::lock_guard<std::mutex> lock(state_mutex);
    if (state) return true;
    state = static_cast<State*>(heap_caps_calloc(
        1, sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!state) {
        ESP_LOGE(TAG, "PSRAM allocation failed");
        return false;
    }
    Settings loaded;
    if (!LoadSettings(loaded) || !ValidSettings(loaded)) {
        ESP_LOGW(TAG, "invalid environment NVS settings; using empty settings");
        loaded = {};
    }
    state->settings = loaded;
    std::memset(loaded.weather_api_key, 0, sizeof(loaded.weather_api_key));
    ESP_LOGI(TAG, "initialized in PSRAM: bytes=%u key_set=%d",
             static_cast<unsigned>(sizeof(State)), state->settings.weather_api_key[0] != 0);
    return true;
}

void SetIdle(bool idle) { idle_allowed.store(idle); }

void Poll() {
    if (!idle_allowed.load()) return;
    wifi_ap_record_t access_point{};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) return;
    const uint64_t now = NowMs();
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (!state || state->worker_active || now < state->next_poll) return;
        if (state->have_network &&
            std::memcmp(state->bssid, access_point.bssid, sizeof(state->bssid)) != 0) {
            state->location_valid = false;
            state->weather_valid = false;
            state->location_http = 0;
            state->weather_http = 0;
        }
        std::memcpy(state->bssid, access_point.bssid, sizeof(state->bssid));
        state->have_network = true;
        const bool need_location = !state->location_valid ||
                                   now - state->location_at >= LOCATION_INTERVAL_MS;
        const bool need_weather = state->settings.weather_api_key[0] &&
                                  (!state->weather_valid ||
                                   now - state->weather_at >= WEATHER_INTERVAL_MS);
        if (!need_location && !need_weather) {
            state->next_poll = NextDueLocked(now);
            return;
        }
        state->worker_active = true;
    }

#if configSUPPORT_STATIC_ALLOCATION && CONFIG_SPIRAM && CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
    TaskHandle_t worker = nullptr;
    if (xTaskCreateWithCaps(Worker, "sc_weather", 12 * 1024, nullptr, 2, &worker,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
        ESP_LOGI(TAG, "PSRAM weather worker started");
        return;
    }
#endif
    std::lock_guard<std::mutex> lock(state_mutex);
    if (state) {
        state->worker_active = false;
        state->next_poll = now + RETRY_INTERVAL_MS;
    }
    ESP_LOGE(TAG, "PSRAM weather worker creation failed");
}

Settings GetSettings() {
    std::lock_guard<std::mutex> lock(state_mutex);
    return state ? state->settings : Settings{};
}

bool SaveSettings(const Settings& value) {
    if (!ValidSettings(value)) return false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t error = nvs_set_str(handle, "api_key", value.weather_api_key);
    if (error == ESP_OK) error = nvs_set_str(handle, "region", value.region_name);
    if (error == ESP_OK) error = nvs_commit(handle);
    nvs_close(handle);
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(error));
        return false;
    }
    stackchan_nvs_commit_observer();
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!state) return false;
    if (std::strcmp(state->settings.weather_api_key, value.weather_api_key) != 0) {
        state->weather_valid = false;
        state->weather_http = 0;
    }
    state->settings = value;
    ++state->revision;
    state->next_poll = 0;
    return true;
}

Snapshot Read() {
    Snapshot result;
    const uint64_t now = NowMs();
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!state) return result;
    result.initialized = true;
    result.location_valid = state->location_valid;
    result.weather_valid = state->weather_valid;
    result.weather_key_set = state->settings.weather_api_key[0] != 0;
    result.fetching = state->worker_active;
    result.location_http = state->location_http;
    result.weather_http = state->weather_http;
    result.latitude = state->location.latitude;
    result.longitude = state->location.longitude;
    std::snprintf(result.city, sizeof(result.city), "%s", state->location.city);
    std::snprintf(result.region, sizeof(result.region), "%s", state->location.region);
    if (state->settings.region_name[0]) {
        std::snprintf(result.region_name, sizeof(result.region_name), "%s",
                      state->settings.region_name);
    } else if (state->location_valid) {
        std::snprintf(result.region_name, sizeof(result.region_name), "%s%s%s",
                      state->location.region,
                      state->location.region[0] && state->location.city[0] &&
                              std::strcmp(state->location.region, state->location.city) != 0 ? " " : "",
                      std::strcmp(state->location.region, state->location.city) != 0
                          ? state->location.city : "");
    }
    if (state->weather_valid) {
        result.temperature_c = state->weather.temperature_c;
        result.humidity_percent = state->weather.humidity_percent;
        result.condition_code = state->weather.condition_code;
        std::snprintf(result.description, sizeof(result.description), "%s",
                      state->weather.description);
        std::snprintf(result.icon, sizeof(result.icon), "%s", state->weather.icon);
        result.weather_age_minutes = now >= state->weather_at
            ? (now - state->weather_at) / 60000 : 0;
        result.weather_stale = now < state->weather_at ||
                               now - state->weather_at >= WEATHER_STALE_MS;
    }
    return result;
}

weather::Appearance ReadAppearance() {
    const uint64_t now = NowMs();
    std::lock_guard<std::mutex> lock(state_mutex);
    if (!state) return {};
    const bool fresh = state->weather_valid && now >= state->weather_at &&
                       now - state->weather_at < WEATHER_STALE_MS;
    return weather::MakeAppearance(state->weather.condition_code,
                                   state->weather.temperature_c,
                                   state->weather.icon[2] == 'n', fresh);
}

} // namespace stackchan::stage1::environment

 
// Called by Xiaozhi only when opening a new speech/conversation WebSocket.
// Mirror S3-stackchan's reference context without a fetch, NVS read or
// change to spoken prompts. Keep Japanese region/weather as reference data.
extern "C" bool stackchan_ws_environment_context(char* output, size_t capacity) {
    if (!output || capacity < 8) return false;
    output[0] = '\0';
    // Format into PSRAM so the WebSocket handshake thread keeps stack headroom.
    struct Scratch {
        stackchan::stage1::environment::Snapshot snapshot;
        char date[160]{};
        char region[260]{};
        char weather[196]{};
        char information[768]{};
    };
    auto scratch = std::unique_ptr<Scratch, decltype(&heap_caps_free)>(
        static_cast<Scratch*>(heap_caps_calloc(1, sizeof(Scratch),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
        heap_caps_free);
    if (!scratch) return false;
    scratch->snapshot = stackchan::stage1::environment::Read();
    const auto& snapshot = scratch->snapshot;
    auto& date = scratch->date;
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (now >= 1704067200 && localtime_r(&now, &local)) {
        constexpr const char* weekdays[] = {
            "日曜日", "月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日"
        };
        char timezone[48]{};
        if (!std::strftime(timezone, sizeof(timezone), "%Z UTC%z", &local))
            std::snprintf(timezone, sizeof(timezone), "本体ローカル時刻");
        std::snprintf(date, sizeof(date),
                      "日時：%d年%d月%d日 %d時%02d分%02d秒（%s）\n曜日：%s\n",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec, timezone,
                      weekdays[local.tm_wday]);
    } else {
        std::snprintf(date, sizeof(date), "日時：時刻が未同期です\n曜日：未取得です\n");
    }

    // Avoid allowing control characters in a configured or IP-derived region
    // name to alter the structure of the reference information.
    auto& region = scratch->region;
    size_t index = 0;
    for (const unsigned char* src =
             reinterpret_cast<const unsigned char*>(snapshot.region_name);
         *src && index + 1 < sizeof(region); ++src) {
        region[index++] = (*src < 0x20 || *src == 0x7f) ? ' ' : static_cast<char>(*src);
    }

    auto& weather = scratch->weather;
    if (snapshot.weather_valid) {
        const auto condition = stackchan::stage1::weather::Classify(snapshot.condition_code);
        std::snprintf(weather, sizeof(weather),
                      "天気：%s%s、気温%.1f度、湿度%uパーセント（%llu分前に取得）\n",
                      snapshot.weather_stale ? "前回の情報・最新情報は未取得：" : "",
                      condition.description, snapshot.temperature_c,
                      snapshot.humidity_percent,
                      static_cast<unsigned long long>(snapshot.weather_age_minutes));
    } else {
        std::snprintf(weather, sizeof(weather), "天気：%s\n",
                      snapshot.weather_key_set ? "まだ取得できていません" :
                                                 "天気のAPIキーが未設定です");
    }

    auto& information = scratch->information;
    const int length = std::snprintf(information, sizeof(information),
                                     "%s地域：%s\n%s",
                                     date, region[0] ? region : "未取得", weather);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(information))
        return false;

    size_t encoded = 0;
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(output), capacity - 1,
                              &encoded,
                              reinterpret_cast<const unsigned char*>(information),
                              static_cast<size_t>(length)) != 0 ||
        encoded >= capacity)
        return false;
    output[encoded] = '\0';
    return true;
}
