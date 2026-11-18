#pragma once
// All S3 sleep/wake/nap speech uses the already-configured Xiaozhi WebSocket
// (12800). No separate HTTP client, audio decoder, TTS service or port.
#include "voice_settings.h"
namespace stackchan::stage1::voice {
bool Request(const Settings& config, const char* kind, const char* text);
bool IsBusy(); // Queued/generating/playing voice; used for S3 scene ordering.
uint8_t SleepSoundPlaying(); // 0=none, 1=snore, 2=sleep talk; actual playback only.
void Cancel(); // Invalidate a stale nap voice on wake/conversation.
}
