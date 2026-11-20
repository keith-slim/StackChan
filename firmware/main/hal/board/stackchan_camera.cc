#include "stackchan_camera.h"
#include "assets/assets.h"
#include "hal_bridge.h"

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
