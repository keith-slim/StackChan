#pragma once
// Stage-1 face / neck / lighting sequencer. All rendering and servo writes run
// under the existing LVGL lock through StackChan::update(). No new task.
#include <cstdint>
#include "stage1_clock.h"
#include "perception_tracking.h"
#include "../avatar/avatar/elements/emotion.h"

namespace stackchan {
class StackChan;
namespace stage1 {
// S3 facial states are separate from the 13-expression swipe UI, which is
// intentionally NOT ported. Calls changing LVGL objects hold the display lock.
enum class ConversationPose : uint8_t { None, Listening, Thinking };
void SetConversationPose(StackChan& stackchan, ConversationPose pose);
void BeginListeningComplete(StackChan& stackchan);
// Normal conversational speech only; all calls hold the existing display lock.
void PrepareSpeechExpression(StackChan& stackchan, const char* source);
void BeginSpeechTurn(StackChan& stackchan);
void StartSpeechEffects(StackChan& stackchan, avatar::Emotion emotion);
void UpdateSpeechEffects(StackChan& stackchan, avatar::Emotion emotion);
void EndSpeechEffects(StackChan& stackchan);
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
bool IsSleepSceneActive(); // Under LVGL lock: shared nap/night preparation and sleep.
void CancelForExternalChange(StackChan& stackchan, const char* source = "unknown"); // Under LVGL lock.
void NotifyManualMotion();  // External commands take ownership of the neck.
void NotifyManualLed();     // External commands take ownership of the LEDs.
bool IsAutonomousSceneActive();
bool IsIdleServoMotionBlocked(); // Under LVGL lock: active scenes + first 3s of each skit.
bool RequestCue(const char* id); // Validated named action; future Live integration.
bool RequestSleep(); // Queue manual sleep until active speech/conversation drains.
// Queue a bounded relative neck correction from Xavier perception. The actual
// servo write is serialized by Update() under the existing LVGL/motion lock.
void RequestWakeSearchPitch(); // Queue absolute 10-degree pitch once per wake.
void StartPersonSearch(int yaw_delta);
void ResumePersonSearch();
void StopPersonSearch();
void StartFacePitchSearch();
void StopFacePitchSearch();
void StopFacePitchSearchOnDetection();
bool IsFacePitchSearchStopping();
bool IsFacePitchSearchRequested();
bool IsPersonSearchActive();
bool IsPersonSearchStopping();
bool IsSpeechTrackingPaused(); // Lock-free for perception worker.
uint32_t PerceptionMotionEpoch();
tracking::PerceptionPose ReadPerceptionPose(uint32_t captured_at, uint32_t epoch);
bool RequestPerceptionLook(int yaw_delta, int pitch_delta, bool face_tracking = false,
                           const tracking::PerceptionPose* pose = nullptr);  // 0.1-degree units.
void Update(StackChan& stackchan);
} // namespace stage1
} // namespace stackchan
