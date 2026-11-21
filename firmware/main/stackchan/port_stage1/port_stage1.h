#pragma once
// Stage-1 face / neck / lighting sequencer. All rendering and servo writes run
// under the existing LVGL lock through StackChan::update(). No new task.
#include <cstdint>
#include "stage1_clock.h"

namespace stackchan {
class StackChan;
namespace stage1 {
// S3 facial states are separate from the 13-expression swipe UI, which is
// intentionally NOT ported. Calls changing LVGL objects hold the display lock.
enum class ConversationPose : uint8_t { None, Listening, Thinking };
void SetConversationPose(StackChan& stackchan, ConversationPose pose);
void BeginListeningComplete(StackChan& stackchan);
bool IsConversationFaceActive();
Config GetConfig(); // Call under LVGL lock after Start().
bool SetConfig(const Config& config); // Persist to NVS and apply; caller holds LVGL lock.
void Start();               // Called only after the Xiaozhi avatar is created.
void Stop();                // Before its LVGL screen is destroyed; caller owns LVGL lock.
void OnStandby(bool idle);  // Status change; does not touch UI objects.
void NotifyExternalSpeech(); // Caller holds LVGL lock: protect newer subtitles.
// Input handlers only enqueue a wake intent; LVGL and servo work stays
// serialized in Update(). true means that the sleep-touch was consumed.
bool RequestWakeTouch(const char* source = "unknown"); // Screen click or head pad press.
bool RequestWakeWord();     // Xiaozhi voice activation/connection.
bool IsWakeSequenceActive(); // Do not let a status/emotion reset erase its face.
bool IsSleepWakeEligible(); // Read-only sleep phase flag for head-pad debounce.
bool IsNapSleepActive(); // Under LVGL lock: preserve nap on background audio error.
bool IsNightBedtimeMessageActive(); // Bedtime TTS's trailing neutral must not abort the sleep sequence.
void CancelForExternalChange(StackChan& stackchan, const char* source = "unknown"); // Under LVGL lock.
void NotifyManualMotion();  // External commands take ownership of the neck.
void NotifyManualLed();     // External commands take ownership of the LEDs.
bool IsAutonomousSceneActive();
bool IsIdleServoMotionBlocked(); // Under LVGL lock: active scenes + first 3s of each skit.
bool RequestCue(const char* id); // Validated named action; future Live integration.
bool RequestSleep(); // Queue manual sleep until active speech/conversation drains.
void Update(StackChan& stackchan);
} // namespace stage1
} // namespace stackchan
