#pragma once
#include <esp_lcd_panel_io.h>
#include <string>
#include <mutex>

namespace stackchan::sd {
// CoreS3 LCD and SD share SPI3 (SCLK=36, MOSI=37) and GPIO35
// (LCD D/C output / SD MISO input). SD access is serialized with LVGL,
// and the display's queued SPI transfers are drained before pin switching.
struct Config {
    esp_lcd_panel_io_handle_t display_io = nullptr;
    std::mutex* camera_mutex = nullptr; // SD skips if capture currently owns it.
    void* power_context = nullptr;
    bool (*set_power)(void*, bool) = nullptr;
};
void Configure(const Config& config);
// Relative paths only; no absolute paths, traversal, or arbitrary mounts.
bool ReadFile(const char* relative_path, std::string& contents);
bool WriteFile(const char* relative_path, const std::string& contents);
// One previous valid generation, using the S3-stackchan stage/rollback scheme.
// Primary: /sdcard/config.json; backup: /sdcard/Backup/config.json.
bool ReadConfig(std::string& contents, bool* recovered = nullptr);
bool WriteConfig(const std::string& contents);
// Compare and, if necessary, replace the snapshot in one mounted session.
// A mount/read error is not an absent file. Validate an existing generation
// before any write or recovery; never overwrite another configuration format.
enum class ConfigSyncResult { Failed, Unchanged, Updated };
ConfigSyncResult SyncConfig(const std::string& contents,
                            bool (*validate_existing)(const std::string&));
} // namespace stackchan::sd
