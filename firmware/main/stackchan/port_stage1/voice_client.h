#pragma once
// Shared Stage-1 speech scheduling and lifecycle, using Xiaozhi's existing
// announcement transport. Scene/dream decisions remain in port_stage1.
#include "voice_settings.h"
#include "stage1_clock.h"
#include <cstdint>

namespace stackchan::stage1::voice {
enum class Kind : uint8_t {
    None, Bedtime, Morning, Touch, Timer, Snore, SleepTalk
};
bool Request(const Settings& config, const char* kind, const char* text, bool manual = false);
bool IsBusy(); // Queued/generating/playing voice; used for scene ordering.
uint8_t SleepSoundPlaying(); // 0=none, 1=snore, 2=sleep talk; actual playback.
void Cancel(); // Invalidate a stale nap voice, not a greeting or conversation.

// All methods below are called by the Stage-1 update under its LVGL lock.
void Reset();
void SceneChanged(Phase phase, uint32_t now);
void QueueScene(Kind kind);
bool Pending();
bool TickScene(const Settings& config, Phase phase, uint32_t now); // true if timer greeting sent
void EnterNap(const Settings& config, uint32_t now);
void LeaveNap();
void Quiet(); // Suppress autonomous voice; does not touch ordinary conversation.
void ResumeNap(const Settings& config, uint32_t now);
bool SleepActive();
Kind FinishSleepIfIdle(const Settings& config, uint32_t now); // None if still playing
bool Due(Kind kind, uint32_t now);
void DiscardDueSleep(const Settings& config, uint32_t now);
bool RequestSleep(Kind kind, const Settings& config, const char* text, uint32_t now);
// S3-style automatic monologue scheduler. It reuses the Stage-1 update task,
// so no additional FreeRTOS task stack is allocated.
bool TickMonologue(const Settings& config, Phase phase, bool eligible, uint32_t now);
}
