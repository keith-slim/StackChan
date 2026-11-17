#include "voice_client.h"
#include <application.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace stackchan::stage1::voice {
namespace {
constexpr char TAG[] = "STAGE1-VOICE";
constexpr size_t MAX_OGG = 160 * 1024;
struct Job {
    char host[128];
    uint16_t port;
    char kind[16];
    char text[513];
    uint32_t serial;
};
struct Response {
    unsigned char* bytes = nullptr;
    size_t len = 0, capacity = 0;
    uint32_t serial = 0;
    bool failed = false;
};
std::atomic<Job*> pending{nullptr};
std::atomic<bool> worker_running{false};
std::atomic<uint32_t> generation{0};
std::atomic<bool> playback_queued{false};
std::atomic<uint8_t> sleep_sound_playing{0};

void FreeJob(Job* job) {
    if (job) heap_caps_free(job);
}
esp_err_t HttpEvent(esp_http_client_event_t* event) {
    if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    auto* response = static_cast<Response*>(event->user_data);
    if (!response || response->serial != generation.load()) return ESP_FAIL;
    if (response->failed || event->data_len < 0 ||
        static_cast<size_t>(event->data_len) > MAX_OGG - response->len) {
        if (response) response->failed = true;
        return ESP_FAIL;
    }
    const size_t next = response->len + static_cast<size_t>(event->data_len);
    if (next > response->capacity) {
        size_t capacity = response->capacity ? response->capacity * 2 : 4096;
        if (capacity < next) capacity = next;
        if (capacity > MAX_OGG) capacity = MAX_OGG;
        auto* newbuf = static_cast<unsigned char*>(
            heap_caps_realloc(response->bytes, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!newbuf) { response->failed = true; return ESP_FAIL; }
        response->bytes = newbuf;
        response->capacity = capacity;
    }
    std::memcpy(response->bytes + response->len, event->data, event->data_len);
    response->len = next;
    return ESP_OK;
}
bool ReadyToPlay() {
    auto& app = Application::GetInstance();
    return app.GetDeviceState() == kDeviceStateIdle &&
           app.GetAudioService().IsPlaybackIdle();
}
void Deliver(Job* job) {
    if (!job || generation.load() != job->serial) return;
    if (!ReadyToPlay()) {
        ESP_LOGI(TAG, "skip %s: conversation or playback active", job->kind);
        return;
    }
    char url[192];
    std::snprintf(url, sizeof(url), "http://%s:%u/speak",
                  job->host, static_cast<unsigned>(job->port));
    // cJSON also quotes strings, but would allocate a temporary dynamic tree.
    // Encode JSON in the fixed stack buffer to avoid internal-heap churn.
    char json[1152];
    size_t n = 0;
    auto add = [&](char c) { if (n < sizeof(json) - 1) json[n++] = c; };
    auto quoted = [&](const char* s) {
        add('"');
        for (; *s; ++s) {
            unsigned char c = static_cast<unsigned char>(*s);
            if (c == '"' || c == '\\') { add('\\'); add(c); }
            else if (c < 0x20) { add(' '); }
            else add(c);
        }
        add('"');
    };
    for (char c: std::string_view("{\"kind\":")) add(c);
    quoted(job->kind);
    for (char c: std::string_view(",\"text\":")) add(c);
    quoted(job->text);
    add('}');
    json[n] = '\0';

    Response response;
    response.serial = job->serial;
    esp_http_client_config_t config{};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 48000;
    config.event_handler = HttpEvent;
    config.user_data = &response;
    config.buffer_size = 2048;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGW(TAG, "HTTP client allocation failed"); return; }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, n);
    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status == 200 && !response.failed &&
        response.len >= 64 && response.serial == generation.load() &&
        std::memcmp(response.bytes, "OggS", 4) == 0 && ReadyToPlay()) {
        // AudioService::PlaySound demuxes Ogg/Opus into the existing decode
        // queue. No second I2S driver, audio task or decoder is created.
        ESP_LOGI(TAG, "play kind=%s ogg_bytes=%u", job->kind,
                 static_cast<unsigned>(response.len));
        sleep_sound_playing.store(std::strcmp(job->kind, "sleep_talk") == 0 ? 2 :
                                  std::strcmp(job->kind, "snore") == 0 ? 1 : 0);
        playback_queued.store(true);
        Application::GetInstance().GetAudioService().PlaySound(
            std::string_view(reinterpret_cast<const char*>(response.bytes),
                             response.len));
    } else {
        ESP_LOGW(TAG, "skip kind=%s HTTP=%d err=%s bytes=%u canceled=%d",
                 job->kind, status, esp_err_to_name(err),
                 static_cast<unsigned>(response.len),
                 response.serial != generation.load());
    }
    if (response.bytes) heap_caps_free(response.bytes);
}
void Worker(void*) {
    for (;;) {
        Job* job = pending.exchange(nullptr);
        if (job) {
            Deliver(job);
            FreeJob(job);
            continue;
        }
        worker_running.store(false);
        if (!pending.load() || worker_running.exchange(true)) break;
    }
    vTaskDelete(nullptr);
}
}
bool IsBusy() {
    // While PlaySound is still queuing packets, IsPlaybackIdle may briefly
    // return true. Never clear the playback ownership flag in that window.
    if (worker_running.load() || pending.load() != nullptr) return true;
    if (playback_queued.load()) {
        if (!Application::GetInstance().GetAudioService().IsPlaybackIdle()) return true;
        playback_queued.store(false);
        sleep_sound_playing.store(0);
    }
    return false;
}
uint8_t SleepSoundPlaying() {
    // IsBusy() also releases the playback ownership when audio drains.
    IsBusy();
    return playback_queued.load() ? sleep_sound_playing.load() : 0;
}
void Cancel() {
    generation.fetch_add(1);
    FreeJob(pending.exchange(nullptr));
    sleep_sound_playing.store(0);
    if (playback_queued.exchange(false)) {
        auto& app = Application::GetInstance();
        // Reset only while the application is not speaking/notifying:
        // waking interrupts a sleep sound, but never clears a reply.
        const auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle || state == kDeviceStateConnecting ||
            state == kDeviceStateListening)
            app.GetAudioService().ResetDecoder();
    }
}
bool Request(const Settings& config, const char* kind, const char* text) {
    if (!config.host[0] || !kind || !text || !text[0]) return false;
    if (std::strlen(kind) >= 16 || std::strlen(text) >= 513) return false;
    // As on S3, sleep sounds never preempt one another.
    const bool sleep_sound = std::strcmp(kind, "snore") == 0 ||
                             std::strcmp(kind, "sleep_talk") == 0;
    if (sleep_sound && IsBusy()) return false;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 18000 ||
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 7000) {
        ESP_LOGW(TAG, "skip voice request: internal RAM guard");
        return false;
    }
    if (!sleep_sound && playback_queued.load()) Cancel();
    auto* job = static_cast<Job*>(heap_caps_malloc(
        sizeof(Job), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!job) { ESP_LOGW(TAG, "skip: PSRAM unavailable"); return false; }
    std::strcpy(job->host, config.host);
    std::strcpy(job->kind, kind);
    std::strcpy(job->text, text);
    job->port = config.port;
    job->serial = generation.fetch_add(1) + 1;
    FreeJob(pending.exchange(job)); // latest scene supersedes obsolete snore
    if (!worker_running.exchange(true)) {
        if (xTaskCreate(Worker, "stage1_voice", 6144, nullptr, 3, nullptr) != pdPASS) {
            worker_running.store(false);
            FreeJob(pending.exchange(nullptr));
            ESP_LOGW(TAG, "voice task not started; preserve existing audio");
            return false;
        }
    }
    ESP_LOGI(TAG, "queued kind=%s server=%s:%u", kind, config.host, config.port);
    return true;
}
}
