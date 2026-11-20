#include "cores3_sd.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <sys/stat.h>

#include <cJSON.h>
#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>

namespace stackchan::sd {
namespace {
constexpr char kTag[] = "CoreS3-SD";
constexpr char kMount[] = "/sdcard";
constexpr char kConfig[] = "/sdcard/config.json";
constexpr char kPrevious[] = "/sdcard/Backup/config.json";
constexpr char kPreviousStage[] = "/sdcard/Backup/config.json.rotate";
constexpr char kBackupDir[] = "/sdcard/Backup";
constexpr size_t kMaxFileBytes = 96 * 1024;
std::mutex storage_mutex;
Config config;

void log_sd_memory(const char* stage) {
    ESP_LOGI(kTag, "[MEM_DIAG] %s internal=%u largest=%u psram=%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

bool safe_name(const char* relative) {
    if (!relative || !*relative || *relative == '/' || std::strstr(relative, "..")) return false;
    for (const char* p = relative; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && c != '_' && c != '-' &&
            c != '.' && c != '/') return false;
    }
    return std::strlen(relative) <= 80;
}

bool read_locked(const std::string& path, std::string& result) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    bool ok = std::fseek(f, 0, SEEK_END) == 0;
    long size = ok ? std::ftell(f) : -1;
    ok = ok && size >= 0 && static_cast<size_t>(size) <= kMaxFileBytes &&
         std::fseek(f, 0, SEEK_SET) == 0;
    if (ok) {
        result.resize(static_cast<size_t>(size));
        if (size != 0) ok = std::fread(result.data(), 1, static_cast<size_t>(size), f) == static_cast<size_t>(size);
    }
    if (std::fclose(f) != 0) ok = false;
    if (!ok) result.clear();
    return ok;
}

bool is_json_object(const std::string& text) {
    cJSON* root = cJSON_ParseWithLength(text.data(), text.size());
    const bool valid = root && cJSON_IsObject(root);
    cJSON_Delete(root);
    return valid;
}

bool verified_write(const std::string& path, const std::string& data) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = (data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size()) &&
              std::fflush(f) == 0;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) return false;
    std::string check;
    return read_locked(path, check) && check == data;
}

// Constructor owns the LVGL lock, pin direction and mount for the WHOLE
// file transaction, including FAT metadata writes and unmount.
class Access {
public:
    Access() : guard_(storage_mutex) {
        log_sd_memory("before_sd_access");
        if (!config.display_io || !config.set_power) return;
        if (config.camera_mutex) {
            camera_guard_ = std::unique_lock<std::mutex>(*config.camera_mutex, std::try_to_lock);
            if (!camera_guard_.owns_lock()) return; // Do not interrupt an active capture.
        }
        // No new LVGL flush can be submitted while this lock is held.
        if (!lvgl_port_lock(5000)) return;
        display_locked_ = true;
        // The SPI LCD implementation drains all queued DMA transfers when
        // tx_param(-1, nullptr, 0) is called (no command is emitted).
        if (esp_lcd_panel_io_tx_param(config.display_io, -1, nullptr, 0) != ESP_OK) return;
        const int before = gpio_get_level(GPIO_NUM_35);
        previous_dc_ = before < 0 ? 0 : before;
        if (gpio_set_direction(GPIO_NUM_35, GPIO_MODE_INPUT) != ESP_OK) return;
        pin_switched_ = true;
        power_attempted_ = true;
        if (!config.set_power(config.power_context, true)) return;
        vTaskDelay(pdMS_TO_TICKS(10));
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI3_HOST;
        host.max_freq_khz = 20000;
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id = SPI3_HOST;
        slot.gpio_cs = GPIO_NUM_4;
        esp_vfs_fat_mount_config_t mount = {};
        mount.format_if_mount_failed = false; // Never format user SD.
        mount.max_files = 5;
        mount.allocation_unit_size = 16 * 1024;
        const esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mount, &card_);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "SD mount unavailable: %s", esp_err_to_name(err));
            return;
        }
        mounted_ = true;
        log_sd_memory("after_sd_mount");
    }
    ~Access() {
        if (mounted_) {
            const esp_err_t err = esp_vfs_fat_sdcard_unmount(kMount, card_);
            if (err != ESP_OK) ESP_LOGE(kTag, "SD unmount error: %s", esp_err_to_name(err));
        }
        if (power_attempted_ && !config.set_power(config.power_context, false))
            ESP_LOGW(kTag, "SD power disable failed");
        if (pin_switched_) {
            // Set the LCD D/C latch before changing the pad back to output.
            gpio_set_level(GPIO_NUM_35, previous_dc_);
            gpio_set_direction(GPIO_NUM_35, GPIO_MODE_OUTPUT);
        }
        if (display_locked_) lvgl_port_unlock();
        log_sd_memory("after_sd_access");
    }
    bool ready() const { return mounted_; }
    Access(const Access&) = delete;
    Access& operator=(const Access&) = delete;
private:
    std::unique_lock<std::mutex> guard_;
    std::unique_lock<std::mutex> camera_guard_;
    bool display_locked_ = false;
    bool pin_switched_ = false;
    bool power_attempted_ = false;
    bool mounted_ = false;
    int previous_dc_ = 0;
    sdmmc_card_t* card_ = nullptr;
};

bool write_config_locked(const std::string& text) {
    if (!is_json_object(text) || text.size() > kMaxFileBytes) return false;
    // A blank, mounted SD must be usable on the FIRST settings snapshot.
    // Build the backup directory even when no earlier config generation exists.
    // Never format the card, and do not invent a previous generation file.
    if (mkdir(kBackupDir, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(kTag, "Could not create %s: %s", kBackupDir, std::strerror(errno));
        return false;
    }
    struct stat backup_dir{};
    if (stat(kBackupDir, &backup_dir) != 0 || !S_ISDIR(backup_dir.st_mode)) {
        ESP_LOGW(kTag, "Backup path is not a directory: %s", kBackupDir);
        return false;
    }
    const std::string stage = std::string(kConfig) + ".stage";
    const std::string rollback = std::string(kConfig) + ".rollback";
    if (!verified_write(stage, text)) {
        std::remove(stage.c_str());
        return false;
    }

    std::string current;
    const bool has_original = read_locked(kConfig, current);
    const bool original_valid = has_original && is_json_object(current);
    // Only these flags are needed below. Free the old JSON before reading
    // the new generation back for verification, avoiding duplicate buffers.
    std::string().swap(current);
    if (has_original) {
        if (std::remove(rollback.c_str()) != 0 && errno != ENOENT) {
            std::remove(stage.c_str());
            return false;
        }
        if (std::rename(kConfig, rollback.c_str()) != 0) {
            std::remove(stage.c_str());
            return false;
        }
    }
    if (std::rename(stage.c_str(), kConfig) != 0) {
        if (has_original) std::rename(rollback.c_str(), kConfig);
        return false;
    }

    std::string confirmed;
    if (!read_locked(kConfig, confirmed) || confirmed != text) {
        std::remove(kConfig);
        if (has_original) std::rename(rollback.c_str(), kConfig);
        return false;
    }

    // Preserve an existing valid backup until the new previous generation
    // has been installed. A failed rename must never silently report success.
    if (has_original && original_valid) {
        struct stat previous_info{};
        const bool has_previous = stat(kPrevious, &previous_info) == 0;
        if (!has_previous && errno != ENOENT) {
            ESP_LOGE(kTag, "Cannot inspect previous backup; retaining rollback");
            return false;
        }
        if (has_previous) {
            // A leftover rotation file means an earlier interrupted update.
            // Do not overwrite it: both generations remain recoverable.
            struct stat rotation_info{};
            if (stat(kPreviousStage, &rotation_info) == 0 || errno != ENOENT) {
                ESP_LOGE(kTag, "Previous backup rotation pending; retaining rollback");
                return false;
            }
            if (std::rename(kPrevious, kPreviousStage) != 0) {
                ESP_LOGE(kTag, "Cannot preserve existing backup; retaining rollback");
                return false;
            }
        }
        if (std::rename(rollback.c_str(), kPrevious) != 0) {
            ESP_LOGE(kTag, "Cannot install new backup; restoring prior backup");
            if (has_previous && std::rename(kPreviousStage, kPrevious) != 0) {
                ESP_LOGE(kTag, "Prior backup remains at %s", kPreviousStage);
            }
            return false;
        }
        if (has_previous && std::remove(kPreviousStage) != 0) {
            ESP_LOGW(kTag, "Old backup remains at %s", kPreviousStage);
        }
    } else if (has_original) {
        std::remove(rollback.c_str());
    }
    return true;
}
} // namespace

void Configure(const Config& next) {
    std::lock_guard<std::mutex> lock(storage_mutex);
    config = next;
}

bool ReadFile(const char* relative, std::string& data) {
    data.clear();
    if (!safe_name(relative)) return false;
    Access access;
    return access.ready() && read_locked(std::string(kMount) + "/" + relative, data);
}
bool WriteFile(const char* relative, const std::string& data) {
    if (!safe_name(relative) || data.size() > kMaxFileBytes) return false;
    Access access;
    if (!access.ready()) return false;
    const std::string base = std::string(kMount) + "/" + relative;
    const std::string stage = base + ".stage";
    if (!verified_write(stage, data)) { std::remove(stage.c_str()); return false; }
    const std::string rollback = base + ".rollback";
    std::remove(rollback.c_str());
    if (std::rename(base.c_str(), rollback.c_str()) != 0 && errno != ENOENT) {
        std::remove(stage.c_str()); return false;
    }
    if (std::rename(stage.c_str(), base.c_str()) != 0) {
        std::rename(rollback.c_str(), base.c_str()); return false;
    }
    return true;
}
bool ReadConfig(std::string& data, bool* recovered) {
    data.clear();
    if (recovered) *recovered = false;
    Access access;
    if (!access.ready()) return false;
    if (read_locked(kConfig, data) && is_json_object(data)) return true;
    data.clear();
    for (const char* path : {kPrevious, kPreviousStage, "/sdcard/config.json.rollback", "/sdcard/config.json.bak"}) {
        if (read_locked(path, data) && is_json_object(data)) {
            if (recovered) *recovered = true;
            // Backups may be healthy even when primary file is damaged.
            if (!write_config_locked(data)) ESP_LOGW(kTag, "Restored config in memory; SD self-repair deferred");
            return true;
        }
        data.clear();
    }
    return false;
}
bool WriteConfig(const std::string& data) {
    Access access;
    return access.ready() && write_config_locked(data);
}
} // namespace stackchan::sd
