#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

class StackChanCamera;
class StackChanAvatarDisplay;

// Keeps the real-time audio path limited to a bounded PSRAM copy. Xavier I/O,
// JPEG encoding and servo requests run in the PSRAM-backed conversation worker.
class StackChanPerception {
public:
    StackChanPerception(StackChanCamera* camera, StackChanAvatarDisplay* display);
    ~StackChanPerception();

    void OnAudioInputFrame(const int16_t* samples, size_t frames, size_t channels, size_t microphone_channels);
    void OnWakeWordDetected();
    void OnConversationEnded();

private:
    static constexpr size_t kSampleRate     = 16000;
    static constexpr size_t kRingFrames     = kSampleRate * 2;
    static constexpr size_t kSnapshotFrames = kSampleRate;

    static void WorkerEntry(void* context);
    void RunWorker(uint64_t snapshot_end);
    bool PostAudio(const int16_t* pcm, size_t frames, float& angle_deg, float& confidence);
    bool PostVision(float& center_x, float& center_y, float& score, float& body_target_x, float& body_target_y, float& body_score, bool& valid_frame);

    StackChanCamera* camera_ = nullptr;
    StackChanAvatarDisplay* display_ = nullptr;
    int16_t* audio_ring_     = nullptr;
    std::atomic<uint64_t> written_frames_{0};
    std::atomic<uint64_t> snapshot_end_{0};
    std::atomic<bool> worker_running_{false};
    std::atomic<bool> stop_requested_{false};
};
