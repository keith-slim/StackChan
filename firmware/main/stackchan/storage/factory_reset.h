#pragma once
#include <esp_err.h>
namespace stackchan::storage {
// Persist the intent before reboot; no configuration is erased while the
// running Wi-Fi, audio or backup tasks can still use it.
esp_err_t RequestFactoryReset();
// Called after NVS init and before board/settings initialization.
esp_err_t CompletePendingFactoryReset();
} // namespace stackchan::storage
