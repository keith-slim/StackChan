#pragma once
// Stage-1 face / neck / lighting sequencer. All rendering and servo writes run
// under the existing LVGL lock through StackChan::update(). No new task.
#include <cstdint>

namespace stackchan {
class StackChan;
namespace stage1 {
void Start();               // Called only after the Xiaozhi avatar is created.
void Stop();                // Before its LVGL screen is destroyed; caller owns LVGL lock.
void OnStandby(bool idle);  // Status change; does not touch UI objects.
void NotifyActivity();      // Can be called from touch/IMU tasks.
void CancelForExternalChange(StackChan& stackchan); // Call under LVGL lock before existing UI commands.
void NotifyManualMotion();  // External commands take ownership of the neck.
void NotifyManualLed();     // External commands take ownership of the LEDs.
bool IsAutonomousSceneActive();
bool RequestCue(const char* id); // Validated named action; future Live integration.
void Update(StackChan& stackchan);
} // namespace stage1
} // namespace stackchan
