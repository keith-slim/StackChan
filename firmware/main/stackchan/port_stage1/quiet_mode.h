#pragma once
#include "quiet_schedule.h"
#include <esp_err.h>

namespace stackchan::stage1::quiet_mode {
void Init();
Status GetStatus();
bool Active();
uint32_t RemainingSeconds();
esp_err_t Apply(const Request& request, bool voice_command = false);
bool SetMinutes(int minutes);
void Tick(); // Detect scheduled activation even during conversation.
}
