#pragma once
#include "esp_video.h"
#include <mutex>

// EspVideo owns capture, format handling, preview, JPEG and HTTP upload.
// StackChan adds only its shutter sound and the existing LAN video interface.
class StackChanCamera : public EspVideo {
    std::mutex* storage_mutex_ = nullptr;
public:
    using EspVideo::EspVideo;
    // Shared with CoreS3 SD operations so capture and storage cannot overlap.
    void SetStorageMutex(std::mutex* mutex) { storage_mutex_ = mutex; }

    bool Capture() override;
    bool StreamCaptures();
    // Silent capture for the local perception worker. JPEG output is allocated
    // in PSRAM and must be released with heap_caps_free().
    bool CaptureJpeg(uint8_t** jpeg, size_t* jpeg_size, uint8_t quality, uint32_t* captured_at = nullptr);

    auto TryLockFrame()
    {
        return std::unique_lock<std::recursive_mutex>(capture_mutex_, std::try_to_lock);
    }
    const uint8_t* GetFrameData() const
    {
        return frame_.data;
    }
    size_t GetFrameSize() const
    {
        return frame_.len;
    }
    int GetFrameWidth() const
    {
        return frame_.width;
    }
    int GetFrameHeight() const
    {
        return frame_.height;
    }
    int GetFrameFormat() const
    {
        return frame_.format;
    }
};
