#pragma once
// Asynchronous, bounded voice transfer to the companion Docker service.
// No new always-on task; scheduled only for S3 sleep/wake/nap events.
#include "voice_settings.h"
namespace stackchan::stage1::voice {
bool Request(const Settings& config, const char* kind, const char* text);
void Cancel(); // Invalidate a stale nap voice on wake/conversation.
}
