#pragma once
// Asynchronous, bounded voice transfer to the companion Docker service.
// No new always-on task; scheduled only for S3 sleep/wake/nap events.
#include "voice_settings.h"
namespace stackchan::stage1::voice {
bool Request(const Settings& config, const char* kind, const char* text);
bool IsBusy(); // Queued/generating/playing voice; used for S3 scene ordering.
uint8_t SleepSoundPlaying(); // 0=none, 1=snore, 2=sleep talk; actual playback only.
void Cancel(); // Invalidate a stale nap voice on wake/conversation.
}
