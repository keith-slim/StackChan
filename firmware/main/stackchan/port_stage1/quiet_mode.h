#pragma once

#include <cstdint>

namespace stackchan::stage1::quiet_mode {
// NVS-backed temporary suppression of autonomous speech only.
// Conversation and explicit wake-word interaction remain available.
void Init();
bool Active();
uint32_t RemainingSeconds();
bool SetMinutes(int minutes); // 0=cancel, 1..1440=temporary silence.
}
