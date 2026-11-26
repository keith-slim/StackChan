#include "stackchan_camera.h"
#include "assets/assets.h"
#include "hal_bridge.h"
#include <hal/hal.h>

bool StackChanCamera::Capture()
{
    std::unique_lock<std::mutex> exclusive;
    if (storage_mutex_) exclusive = std::unique_lock<std::mutex>(*storage_mutex_);
    hal_bridge::app_play_sound(OGG_CAMERA_SHUTTER);
    return EspVideo::Capture();
}

bool StackChanCamera::StreamCaptures()
{
    std::unique_lock<std::mutex> exclusive;
    if (storage_mutex_) exclusive = std::unique_lock<std::mutex>(*storage_mutex_);
    return CaptureFrame(false, 0);
}

bool StackChanCamera::CaptureJpeg(uint8_t** jpeg, size_t* jpeg_size, uint8_t quality, uint32_t* captured_at)
{
    if (!jpeg || !jpeg_size) return false;
    *jpeg      = nullptr;
    *jpeg_size = 0;
    std::unique_lock<std::mutex> exclusive;
    if (storage_mutex_) exclusive = std::unique_lock<std::mutex>(*storage_mutex_);
    std::lock_guard<std::recursive_mutex> frame_lock(capture_mutex_);
    if (!CaptureFrame(false, 1)) return false;
    // Timestamp the completed frame before JPEG encoding or network I/O.
    if (captured_at) *captured_at = GetHAL().millis();
    return image_to_jpeg(frame_.data, frame_.len, frame_.width, frame_.height, frame_.format, quality, jpeg, jpeg_size);
}
