#include "stackchan_perception.h"

#include "sdkconfig.h"

#include "board.h"
#include "stackchan_camera.h"
#include "stackchan/port_stage1/port_stage1.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>
#include <utility>

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace {
constexpr char kTag[]                 = "Perception";
constexpr int kDoaTimeoutMs           = 1800;
constexpr int kVisionTimeoutMs        = 5000;
constexpr int kServoSettleMs          = 600;
constexpr float kMinimumDoaConfidence = 0.04f;
constexpr float kMinimumFaceScore     = 0.55f;

std::string Endpoint(const char* path)
{
    std::string endpoint = CONFIG_STACKCHAN_PERCEPTION_URL;
    while (!endpoint.empty() && endpoint.back() == '/') endpoint.pop_back();
    endpoint += path;
    return endpoint;
}

bool Post(const std::string& endpoint, const char* content_type, const void* body, size_t body_size, int timeout_ms,
          const std::initializer_list<std::pair<const char*, const char*>>& headers, std::string& response)
{
    response.clear();
    auto network = Board::GetInstance().GetNetwork();
    if (!network || body == nullptr || body_size == 0) return false;
    auto http = network->CreateHttp(3);
    if (!http) return false;

    http->SetTimeout(timeout_ms);
    http->SetHeader("Content-Type", content_type);
    for (const auto& header : headers) http->SetHeader(header.first, header.second);

    // HttpClient adds Content-Length for SetContent. The body and request
    // buffers exceed the allocator threshold and therefore live in PSRAM.
    http->SetContent(std::string(static_cast<const char*>(body), body_size));
    if (auto opened = http->Open("POST", endpoint); !opened) {
        ESP_LOGW(kTag, "POST open failed: %s", endpoint.c_str());
        http->Close();
        return false;
    }
    const auto status = http->GetStatusCode();
    if (!status || *status != 200) {
        ESP_LOGW(kTag, "POST failed: endpoint=%s status=%d", endpoint.c_str(), status ? *status : -1);
        http->Close();
        return false;
    }
    response = http->ReadAll();
    http->Close();
    return !response.empty();
}

bool JsonNumber(cJSON* object, const char* name, float& value)
{
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) return false;
    value = static_cast<float>(item->valuedouble);
    return true;
}
}  // namespace

StackChanPerception::StackChanPerception(StackChanCamera* camera) : camera_(camera)
{
    audio_ring_ =
        static_cast<int16_t*>(heap_caps_calloc(kRingFrames * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!audio_ring_) ESP_LOGE(kTag, "Could not allocate dual-mic PSRAM ring");
}

StackChanPerception::~StackChanPerception()
{
    heap_caps_free(audio_ring_);
}

void StackChanPerception::OnAudioInputFrame(const int16_t* samples, size_t frames, size_t channels,
                                            size_t microphone_channels)
{
    if (!audio_ring_ || !samples || channels < 2 || microphone_channels < 2) return;
    const uint64_t begin = written_frames_.load(std::memory_order_relaxed);
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t destination     = static_cast<size_t>((begin + frame) % kRingFrames) * 2;
        const size_t source          = frame * channels;
        audio_ring_[destination]     = samples[source];
        audio_ring_[destination + 1] = samples[source + 1];
    }
    written_frames_.store(begin + frames, std::memory_order_release);
}

void StackChanPerception::OnWakeWordDetected()
{
    if (!audio_ring_ || !camera_) return;
    bool expected = false;
    if (!worker_running_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ESP_LOGI(kTag, "Wake perception already running; keeping conversation path unblocked");
        return;
    }
    snapshot_end_.store(written_frames_.load(std::memory_order_acquire), std::memory_order_release);
    TaskHandle_t task = nullptr;
    if (xTaskCreateWithCaps(WorkerEntry, "perception", 12288, this, 2, &task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        worker_running_.store(false, std::memory_order_release);
        ESP_LOGW(kTag, "Could not start PSRAM perception worker");
    }
}

void StackChanPerception::WorkerEntry(void* context)
{
    auto* self = static_cast<StackChanPerception*>(context);
    self->RunWorker(self->snapshot_end_.load(std::memory_order_acquire));
    self->worker_running_.store(false, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

void StackChanPerception::RunWorker(uint64_t snapshot_end)
{
    const size_t available = static_cast<size_t>(std::min<uint64_t>(snapshot_end, kSnapshotFrames));
    int16_t* snapshot      = nullptr;
    if (available >= 256) {
        snapshot = static_cast<int16_t*>(
            heap_caps_malloc(available * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (snapshot) {
        const uint64_t first = snapshot_end - available;
        for (size_t frame = 0; frame < available; ++frame) {
            const size_t source     = static_cast<size_t>((first + frame) % kRingFrames) * 2;
            snapshot[frame * 2]     = audio_ring_[source];
            snapshot[frame * 2 + 1] = audio_ring_[source + 1];
        }
        float angle      = 0.0f;
        float confidence = 0.0f;
        if (PostAudio(snapshot, available, angle, confidence)) {
            ESP_LOGI(kTag, "DOA angle=%.1f confidence=%.3f frames=%u", angle, confidence,
                     static_cast<unsigned>(available));
            if (confidence >= kMinimumDoaConfidence) {
                const int yaw_delta = static_cast<int>(std::clamp(angle, -70.0f, 70.0f) * 10.0f);
                if (stackchan::stage1::RequestPerceptionLook(yaw_delta, 0))
                    vTaskDelay(pdMS_TO_TICKS(kServoSettleMs));
            }
        }
        heap_caps_free(snapshot);
    } else {
        ESP_LOGW(kTag, "No dual-mic wake snapshot available");
    }

    float center_x = 0.5f;
    float center_y = 0.5f;
    float score    = 0.0f;
    if (!PostVision(center_x, center_y, score) || score < kMinimumFaceScore) {
        ESP_LOGI(kTag, "No face in DOA view; keeping audio direction");
        return;
    }

    // CoreS3の実機ではyaw負方向がカメラ画像の左、pitch正方向が上を向く。
    const int yaw_delta   = static_cast<int>(std::clamp((center_x - 0.5f) * 600.0f, -250.0f, 250.0f));
    const int pitch_delta = static_cast<int>(std::clamp((0.5f - center_y) * 400.0f, -160.0f, 160.0f));
    ESP_LOGI(kTag, "Face center=(%.3f,%.3f) score=%.3f correction=(%d,%d)", center_x, center_y, score, yaw_delta,
             pitch_delta);
    if (std::abs(yaw_delta) >= 20 || std::abs(pitch_delta) >= 20)
        stackchan::stage1::RequestPerceptionLook(yaw_delta, pitch_delta);
}

bool StackChanPerception::PostAudio(const int16_t* pcm, size_t frames, float& angle_deg, float& confidence)
{
    std::string response;
    if (!Post(Endpoint("/v1/audio/doa"), "application/octet-stream", pcm, frames * 2 * sizeof(int16_t), kDoaTimeoutMs,
              {{"X-Sample-Rate", "16000"}, {"X-Channels", "2"}}, response))
        return false;
    cJSON* root   = cJSON_ParseWithLength(response.data(), response.size());
    const bool ok = root && JsonNumber(root, "angle_deg", angle_deg) && JsonNumber(root, "confidence", confidence);
    cJSON_Delete(root);
    return ok;
}

bool StackChanPerception::PostVision(float& center_x, float& center_y, float& score)
{
    uint8_t* jpeg    = nullptr;
    size_t jpeg_size = 0;
    if (!camera_->CaptureJpeg(&jpeg, &jpeg_size, 55) || !jpeg || jpeg_size == 0) return false;
    std::string response;
    const bool posted =
        Post(Endpoint("/v1/vision/detect"), "image/jpeg", jpeg, jpeg_size, kVisionTimeoutMs, {}, response);
    heap_caps_free(jpeg);
    if (!posted) return false;

    cJSON* root      = cJSON_ParseWithLength(response.data(), response.size());
    cJSON* faces     = root ? cJSON_GetObjectItemCaseSensitive(root, "faces") : nullptr;
    cJSON* best      = nullptr;
    float best_score = -1.0f;
    if (cJSON_IsArray(faces)) {
        cJSON* face = nullptr;
        cJSON_ArrayForEach(face, faces)
        {
            float candidate = 0.0f;
            if (JsonNumber(face, "score", candidate) && candidate > best_score) {
                best       = face;
                best_score = candidate;
            }
        }
    }
    bool ok       = false;
    cJSON* center = best ? cJSON_GetObjectItemCaseSensitive(best, "center") : nullptr;
    if (cJSON_IsArray(center) && cJSON_GetArraySize(center) >= 2) {
        cJSON* x = cJSON_GetArrayItem(center, 0);
        cJSON* y = cJSON_GetArrayItem(center, 1);
        if (cJSON_IsNumber(x) && cJSON_IsNumber(y)) {
            center_x = static_cast<float>(x->valuedouble);
            center_y = static_cast<float>(y->valuedouble);
            score    = best_score;
            ok       = std::isfinite(center_x) && std::isfinite(center_y);
        }
    }
    cJSON_Delete(root);
    return ok;
}
