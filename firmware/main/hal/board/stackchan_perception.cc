#include "stackchan_perception.h"
#include <stackchan/port_stage1/perception_mailbox.h>
#include <stackchan/port_stage1/face_filter_guard.h>

#include "sdkconfig.h"

#include "application.h"
#include "board.h"
#include "stackchan_camera.h"
#include "stackchan_display.h"
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
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

namespace {
constexpr char kTag[]                 = "Perception";
constexpr int kDoaTimeoutMs           = 1800;
constexpr int kVisionTimeoutMs        = 5000;
constexpr int kTrackingIntervalMs     = 250;
constexpr int kTrackingStartGraceMs   = 3000;
constexpr int kTrackingMaxMs          = 5 * 60 * 1000;
constexpr float kMinimumDoaConfidence = 0.04f;
constexpr float kMinimumFaceScore     = 0.55f;
constexpr float kMinimumBodyScore     = 0.50f;

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

StackChanPerception::StackChanPerception(StackChanCamera* camera, StackChanAvatarDisplay* display)
    : camera_(camera), display_(display)
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
    stop_requested_.store(false, std::memory_order_release);
    stackchan::stage1::RequestWakeSearchPitch();
    // Only an actual wake from idle can start a pre-conversation greeting.
    if (display_ && Application::GetInstance().GetDeviceState() == kDeviceStateIdle)
        display_->ArmWakeFaceSmile();
    snapshot_end_.store(written_frames_.load(std::memory_order_acquire), std::memory_order_release);
    TaskHandle_t task = nullptr;
    if (xTaskCreateWithCaps(WorkerEntry, "perception", 12288, this, 2, &task, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
        pdPASS) {
        worker_running_.store(false, std::memory_order_release);
        ESP_LOGW(kTag, "Could not start PSRAM perception worker");
    }
}

void StackChanPerception::OnConversationEnded()
{
    stop_requested_.store(true, std::memory_order_release);
    stackchan::stage1::StopPersonSearch();
    ESP_LOGI(kTag, "Conversation ended; stopping face tracking");
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
                stackchan::stage1::StartPersonSearch(yaw_delta);
            }
        }
        heap_caps_free(snapshot);
    } else {
        ESP_LOGW(kTag, "No dual-mic wake snapshot available");
    }

    const int64_t tracking_started_us = esp_timer_get_time();
    bool conversation_seen            = false;
    bool person_missing_logged        = false;
    bool face_filter_initialized      = false;
    bool wake_face_smile_shown         = false;
    bool face_center_locked           = false;
    stackchan::stage1::FacePitchHold face_pitch;
    int centered_frames               = 0;
    int face_missing_frames           = 0;
    int person_missing_frames         = 0;
    bool person_acquired              = false;
    bool face_sweep_attempted          = false;
    float filtered_center_x           = 0.5f;
    float filtered_center_y           = 0.5f;
    ESP_LOGI(kTag, "Realtime face tracking started");
    while (!stop_requested_.load(std::memory_order_acquire) &&
           (esp_timer_get_time() - tracking_started_us) / 1000 < kTrackingMaxMs) {
        const int64_t frame_started_us = esp_timer_get_time();
        auto& app = Application::GetInstance();
        const bool wake_handoff = app.IsWakeWordHandoffPending();
        const auto state = app.GetDeviceState();
        const bool conversation_active = !app.IsAnnouncementActive() &&
            (state == kDeviceStateConnecting || state == kDeviceStateListening ||
             state == kDeviceStateSpeaking || state == kDeviceStateNotifying);
        conversation_seen = conversation_seen || conversation_active;
        const int64_t tracking_elapsed_ms = (frame_started_us - tracking_started_us) / 1000;
        if (!wake_handoff && ((conversation_seen && state == kDeviceStateIdle) ||
            (!conversation_seen && tracking_elapsed_ms >= kTrackingStartGraceMs)))
            break;

        // 独り言から会話へつなぐ際の一時的な待機で追従を終了させない。
        if (wake_handoff && state == kDeviceStateIdle) {
            vTaskDelay(pdMS_TO_TICKS(kTrackingIntervalMs));
            continue;
        }

        // Keep audio transport bandwidth available during playback and
        // avoid tracking a deliberately moving expression pose.
        if (state == kDeviceStateSpeaking || stackchan::stage1::IsSpeechTrackingPaused()) {
            stackchan::stage1::NoteCenteredFace(false, 0);
            face_filter_initialized = false;
            face_center_locked = false;
            face_pitch.Reset();
            centered_frames = 0;
            face_missing_frames = 0;
            vTaskDelay(pdMS_TO_TICKS(kTrackingIntervalMs));
            continue;
        }

        float center_x = 0.5f;
        float center_y = 0.5f;
        float score    = 0.0f;
        float face_height = 0.0f;
        float body_target_x = 0.5f;
        float body_target_y = 0.5f;
        float body_score    = 0.0f;
        const bool was_searching = stackchan::stage1::IsPersonSearchActive();
        const uint32_t generation = stackchan::stage1::PerceptionGeneration();
        const uint32_t captured = static_cast<uint32_t>(frame_started_us / 1000);
        bool valid_frame = false;
        const bool face_detected = PostVision(center_x, center_y, score, face_height,
                                             body_target_x, body_target_y, body_score, valid_frame);
        // A request already in flight may complete after speech starts.
        if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking ||
            stackchan::stage1::IsSpeechTrackingPaused()) {
            face_pitch.Reset();
            continue;
        }
        if (stop_requested_.load(std::memory_order_acquire)) break;
        if (generation != stackchan::stage1::PerceptionGeneration() ||
            !stackchan::stage1::IsPerceptionFresh(static_cast<uint32_t>(esp_timer_get_time() / 1000), captured) || !valid_frame) {
            person_missing_frames = 0;
            face_pitch.ResetMotion();
            vTaskDelay(pdMS_TO_TICKS(kTrackingIntervalMs));
            continue;
        }
        if (stackchan::stage1::IsPersonSearchActive()) {
            face_pitch.Reset();
            if (valid_frame && ((face_detected && score >= kMinimumFaceScore) || body_score >= kMinimumBodyScore)) {
                const bool found_face = face_detected && score >= kMinimumFaceScore;
                stackchan::stage1::StopPersonSearch();
                person_acquired = true;
                person_missing_frames = 0;
                if (!found_face && !face_sweep_attempted) {
                    stackchan::stage1::StartFacePitchSearch();
                    face_sweep_attempted = true;
                }
                ESP_LOGI(kTag, "[PERSON_SEARCH] found %s", found_face ? "face" : "body");
                face_filter_initialized = false;
                face_center_locked = false;
                centered_frames = 0;
                face_missing_frames = 0;
            }
            // No tracking corrections from a frame captured during a sweep.
            // Use a fresh image after the stop is applied by Stage1.
            vTaskDelay(pdMS_TO_TICKS(kTrackingIntervalMs));
            continue;
        }
        if (was_searching || stackchan::stage1::IsPersonSearchStopping()) {
            face_pitch.Reset();
            vTaskDelay(pdMS_TO_TICKS(kTrackingIntervalMs));
            continue;
        }
        const bool tracking_face = face_detected && score >= kMinimumFaceScore;
        if (!tracking_face) {
            face_pitch.ResetMotion();
            stackchan::stage1::NoteCenteredFace(false, 0);
        }
        if (tracking_face) {
            if (stackchan::stage1::IsFacePitchSearchRequested()) {
                stackchan::stage1::StopFacePitchSearch();
                ESP_LOGI(kTag, "[FACE_PITCH_SEARCH] face found score=%.3f", score);
            }
            person_missing_logged = false;
            person_acquired = true;
            person_missing_frames = 0;
            face_missing_frames   = 0;
            if (!wake_face_smile_shown && display_ && !stop_requested_.load(std::memory_order_acquire))
                wake_face_smile_shown = display_->ShowWakeFaceSmile();
            // 距離や検出枠の揺れをそのままサーボへ渡さず、中心座標を平滑化する。
            constexpr float kFilterAlpha = 0.35f;
            if (!face_filter_initialized) {
                filtered_center_x      = center_x;
                filtered_center_y      = center_y;
                face_filter_initialized = true;
            } else {
                filtered_center_x += (center_x - filtered_center_x) * kFilterAlpha;
                filtered_center_y += (center_y - filtered_center_y) * kFilterAlpha;
            }
            const float filtered_x = filtered_center_x - 0.5f;
            const float filtered_y = 0.5f - filtered_center_y;
            const float raw_x = center_x - 0.5f;
            const float raw_y = 0.5f - center_y;
            const bool face_centered = stackchan::stage1::FaceCenterEligible(raw_x, raw_y, filtered_x, filtered_y);
            stackchan::stage1::NoteCenteredFace(face_centered, captured);
            const float error_x = stackchan::stage1::GuardedFaceError(raw_x, filtered_x, 0.025f);
            const float error_y = face_pitch.Update(raw_y, filtered_y, face_height);
            // 左右は顔が3フレーム中央にあれば固定する。上下は独立した保持条件を使う。
            if (face_center_locked) {
                if (stackchan::stage1::FaceCenterLost(raw_x, raw_y, filtered_x, filtered_y)) {
                    face_center_locked = false;
                    centered_frames    = 0;
                }
            } else if (face_centered) {
                if (++centered_frames >= 3) face_center_locked = true;
            } else {
                centered_frames = 0;
            }
            // CoreS3実機のyaw負方向が画像の左、pitch正方向が画像の上を向く。
            const int yaw_delta = face_center_locked || std::abs(error_x) < 0.025f
                                      ? 0
                                      : static_cast<int>(std::clamp(error_x * 500.0f, -160.0f, 160.0f));
            const int pitch_delta = static_cast<int>(std::clamp(error_y * 360.0f, -110.0f, 110.0f));
            ESP_LOGI(kTag, "Track face=(%.3f,%.3f) filtered=(%.3f,%.3f) score=%.3f locked=%d correction=(%d,%d) face_h=%.3f pitch_band=%.3f",
                     center_x, center_y, filtered_center_x, filtered_center_y, score, face_center_locked,
                     yaw_delta, pitch_delta, face_height, face_pitch.ResumeBand());
            stackchan::stage1::RequestPerceptionLook(yaw_delta, pitch_delta, true, captured, generation);
        } else if (stackchan::stage1::IsFacePitchSearchRequested()) {
            // Keep acquiring faces while pitch sweeps; don't replace the sweep
            // with a zero/deadband body correction or a transient missing frame.
        } else if (body_score >= kMinimumBodyScore) {
            person_missing_logged = false;
            person_acquired = true;
            person_missing_frames = 0;
            ++face_missing_frames;
            // 顔検出が一瞬抜けただけでは身体追従へ切り替えず、上下の往復を防ぐ。
            if (face_missing_frames >= 12) {
                face_filter_initialized = false;
                face_pitch.Reset();
                face_center_locked      = false;
                centered_frames         = 0;
            }
            if (face_missing_frames >= 4) {
                const float error_x = body_target_x - 0.5f;
                const float error_y = 0.5f - body_target_y;
                const int yaw_delta = std::abs(error_x) < 0.05f ? 0
                    : static_cast<int>(std::clamp(error_x * 500.0f, -160.0f, 160.0f));
                const int pitch_delta = std::abs(error_y) < 0.08f
                                            ? 0
                                            : static_cast<int>(std::clamp(error_y * 300.0f, -100.0f, 100.0f));
                ESP_LOGI(kTag, "Track body head_hint=(%.3f,%.3f) score=%.3f correction=(%d,%d)",
                         body_target_x, body_target_y, body_score, yaw_delta, pitch_delta);
                stackchan::stage1::RequestPerceptionLook(yaw_delta, pitch_delta, false, captured, generation);
            }
        } else {
            ++face_missing_frames;
            if (face_missing_frames >= 12) {
                face_filter_initialized = false;
                face_pitch.Reset();
                face_center_locked      = false;
                centered_frames         = 0;
            }
            if (!person_missing_logged) {
                ESP_LOGI(kTag, "No face or body in view; keeping audio direction and tracking");
                person_missing_logged = true;
            }
            if (valid_frame && person_acquired) {
                stackchan::stage1::RequestPerceptionLook(0, 0, false, captured, generation);
                if (++person_missing_frames >= 4 && !stop_requested_.load(std::memory_order_acquire) &&
                    Application::GetInstance().GetDeviceState() == kDeviceStateListening) {
                    person_acquired = false; // one bounded sweep per loss, not endless retries
                    person_missing_frames = 0;
                    ESP_LOGI(kTag, "[PERSON_SEARCH] restart: person lost");
                    stackchan::stage1::ResumePersonSearch();
                }
            } else if (!valid_frame) {
                person_missing_frames = 0;
            }
        }

        const int64_t frame_elapsed_ms = (esp_timer_get_time() - frame_started_us) / 1000;
        if (frame_elapsed_ms < kTrackingIntervalMs)
            vTaskDelay(pdMS_TO_TICKS(kTrackingIntervalMs - frame_elapsed_ms));
    }
    stackchan::stage1::NoteCenteredFace(false, 0);
    stackchan::stage1::StopPersonSearch();
    ESP_LOGI(kTag, "Realtime face tracking stopped");
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

bool StackChanPerception::PostVision(float& center_x, float& center_y, float& score, float& face_height,
                                     float& body_target_x, float& body_target_y, float& body_score, bool& valid_frame)
{
    valid_frame = false;
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
    valid_frame = cJSON_IsArray(faces) || (root && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "bodies")));
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
    float image_height = 0.0f;
    if (ok && JsonNumber(root, "height", image_height)) {
        cJSON* box = cJSON_GetObjectItemCaseSensitive(best, "bbox");
        if (cJSON_IsArray(box)) {
            cJSON* top = cJSON_GetArrayItem(box, 1);
            cJSON* bottom = cJSON_GetArrayItem(box, 3);
            if (cJSON_IsNumber(top) && cJSON_IsNumber(bottom))
                face_height = stackchan::stage1::NormalizedFaceHeight(top->valuedouble, bottom->valuedouble, image_height);
        }
    }
    if (!ok || score < kMinimumFaceScore) {
        cJSON* bodies = root ? cJSON_GetObjectItemCaseSensitive(root, "bodies") : nullptr;
        cJSON* body   = nullptr;
        float best_body_score = -1.0f;
        if (cJSON_IsArray(bodies)) {
            cJSON* candidate_body = nullptr;
            cJSON_ArrayForEach(candidate_body, bodies)
            {
                float candidate_score = 0.0f;
                if (JsonNumber(candidate_body, "score", candidate_score) && candidate_score > best_body_score) {
                    body            = candidate_body;
                    best_body_score = candidate_score;
                }
            }
        }
        cJSON* head_hint = body ? cJSON_GetObjectItemCaseSensitive(body, "head_hint") : nullptr;
        if (cJSON_IsArray(head_hint) && cJSON_GetArraySize(head_hint) >= 2) {
            cJSON* x = cJSON_GetArrayItem(head_hint, 0);
            cJSON* y = cJSON_GetArrayItem(head_hint, 1);
            if (cJSON_IsNumber(x) && std::isfinite(x->valuedouble) &&
                cJSON_IsNumber(y) && std::isfinite(y->valuedouble)) {
                body_target_x = static_cast<float>(x->valuedouble);
                body_target_y = static_cast<float>(y->valuedouble);
                body_score    = best_body_score;
            }
        }
    }
    cJSON_Delete(root);
    return ok;
}
