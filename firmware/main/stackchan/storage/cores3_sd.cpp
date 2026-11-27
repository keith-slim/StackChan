#include "cores3_sd.h"
#include "hal/board/config.h"

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
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>
#include <esp_timer.h>
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
        // GPIO35 is LCD D/C while drawing and SPI3 MISO while reading SD.
        // The LCD driver selects D/C for every subsequent transfer, so there
        // is no need to sample the old output with the input buffer disabled.
        if (gpio_set_direction(SD_SPI_MISO_GPIO, GPIO_MODE_INPUT) != ESP_OK) return;
        pin_switched_ = true;
        esp_rom_gpio_connect_in_signal(SD_SPI_MISO_GPIO, SPI3_Q_IN_IDX, false);
        // The SDSPI driver reconfigures and reserves CS on each mount.
        // Reset our exclusively owned CS pad to release a previous reservation.
        // Set the high latch before enabling output; do not select a card
        // while its supply is rising.
        cs_touched_ = true;
        if (gpio_reset_pin(SD_SPI_CS_GPIO) != ESP_OK ||
            gpio_set_pull_mode(SD_SPI_CS_GPIO, GPIO_FLOATING) != ESP_OK) {
            ESP_LOGW(kTag, "SD CS preparation failed");
            return;
        }
        power_attempted_ = true;
        // Ensure a defined off/on transition, including after a failed mount.
        if (!config.set_power(config.power_context, false)) {
            ESP_LOGW(kTag, "SD power reset failed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20) + 1);
        // Keep CS high-impedance during the off interval to avoid feeding
        // the unpowered card through CS. Deselect immediately before power-on.
        if (gpio_set_level(SD_SPI_CS_GPIO, 1) != ESP_OK ||
            gpio_set_direction(SD_SPI_CS_GPIO, GPIO_MODE_INPUT_OUTPUT) != ESP_OK) {
            ESP_LOGW(kTag, "SD CS deselect failed");
            return;
        }
        if (!config.set_power(config.power_context, true)) {
            ESP_LOGW(kTag, "SD power enable failed");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50) + 1);
        ESP_LOGI(kTag, "SD power ready: CS=%d MISO=%d SPI3",
                 gpio_get_level(SD_SPI_CS_GPIO), gpio_get_level(SD_SPI_MISO_GPIO));
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        host.slot = SPI3_HOST;
        host.max_freq_khz = 20000;
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.host_id = SPI3_HOST;
        slot.gpio_cs                     = SD_SPI_CS_GPIO;
        esp_vfs_fat_mount_config_t mount = {};
        mount.format_if_mount_failed = false; // Never format user SD.
        mount.max_files = 5;
        mount.allocation_unit_size = 16 * 1024;
        const int64_t mount_started = esp_timer_get_time();
        const esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mount, &card_);
        const unsigned mount_ms = static_cast<unsigned>((esp_timer_get_time() - mount_started) / 1000);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "SD mount unavailable: %s (%u ms)", esp_err_to_name(err), mount_ms);
            return;
        }
        mounted_ = true;
        ESP_LOGI(kTag, "SD mounted in %u ms", mount_ms);
        log_sd_memory("after_sd_mount");
    }
    ~Access() { Close(); }
    bool Close() {
        if (closed_) return close_ok_;
        closed_ = true;
        if (mounted_) {
            const esp_err_t err = esp_vfs_fat_sdcard_unmount(kMount, card_);
            mounted_ = false;
            if (err != ESP_OK) {
                ESP_LOGE(kTag, "SD unmount error: %s", esp_err_to_name(err));
                close_ok_ = false;
            }
        }
        // No further SPI transactions are permitted. Release the SD CS
        // reservation before the next mount, and avoid driving an unpowered card.
        if (cs_touched_) {
            if (gpio_reset_pin(SD_SPI_CS_GPIO) != ESP_OK ||
                gpio_set_pull_mode(SD_SPI_CS_GPIO, GPIO_FLOATING) != ESP_OK) {
                ESP_LOGW(kTag, "SD CS release failed");
                close_ok_ = false;
            }
        }
        if (power_attempted_ && !config.set_power(config.power_context, false)) {
            ESP_LOGW(kTag, "SD power disable failed");
            close_ok_ = false;
        }
        if (pin_switched_) {
            // LCD CS is inactive; restore D/C as a plain GPIO output.
            // The next panel transaction selects its own command/data level.
            esp_rom_gpio_connect_out_signal(SD_SPI_MISO_GPIO, SIG_GPIO_OUT_IDX, false, false);
            if (gpio_set_level(SD_SPI_MISO_GPIO, 1) != ESP_OK ||
                gpio_set_direction(SD_SPI_MISO_GPIO, GPIO_MODE_OUTPUT) != ESP_OK) {
                ESP_LOGW(kTag, "LCD D/C restore failed");
                close_ok_ = false;
            }
        }
        if (display_locked_) lvgl_port_unlock();
        log_sd_memory("after_sd_access");
        return close_ok_;
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
    bool cs_touched_ = false;
    bool closed_ = false;
    bool close_ok_ = true;
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
    if (!has_original) {
        struct stat info{};
        if (stat(kConfig, &info) == 0 || errno != ENOENT) {
            ESP_LOGW(kTag, "Existing config could not be read; retaining it");
            std::remove(stage.c_str());
            return false;
        }
    }
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
namespace {
enum class ConfigRead { Missing, Found, Invalid, Error };
ConfigRead read_config_locked(std::string& data, bool& recovered, bool strict) {
    data.clear();
    recovered = false;
    bool invalid = false;
    bool io_error = false;
    for (const char* path : {kConfig, kPrevious, kPreviousStage,
                            "/sdcard/config.json.rollback", "/sdcard/config.json.bak"}) {
        struct stat info{};
        if (stat(path, &info) != 0) {
            if (errno == ENOENT) continue;
            io_error = true;
            if (strict) return ConfigRead::Error;
            continue;
        }
        if (!read_locked(path, data)) {
            io_error = true;
            if (strict) return ConfigRead::Error;
            continue;
        }
        if (is_json_object(data)) {
            recovered = std::strcmp(path, kConfig) != 0;
            return ConfigRead::Found;
        }
        invalid = true;
        data.clear();
    }
    if (io_error) return ConfigRead::Error;
    return invalid ? ConfigRead::Invalid : ConfigRead::Missing;
}
} // namespace

bool ReadConfig(std::string& data, bool* recovered) {
    data.clear();
    if (recovered) *recovered = false;
    Access access;
    if (!access.ready()) return false;
    bool from_backup = false;
    if (read_config_locked(data, from_backup, false) != ConfigRead::Found) return false;
    if (recovered) *recovered = from_backup;
    if (from_backup && !write_config_locked(data))
        ESP_LOGW(kTag, "Restored config in memory; SD self-repair deferred");
    return true;
}
bool WriteConfig(const std::string& data) {
    Access access;
    if (!access.ready()) return false;
    const bool ok = write_config_locked(data);
    return access.Close() && ok;
}

ConfigSyncResult SyncConfig(const std::string& data,
                            bool (*validate_existing)(const std::string&)) {
    if (!validate_existing || !is_json_object(data) || data.size() > kMaxFileBytes)
        return ConfigSyncResult::Failed;
    Access access;
    if (!access.ready()) return ConfigSyncResult::Failed;
    std::string previous;
    bool recovered = false;
    const auto read = read_config_locked(previous, recovered, true);
    if (read == ConfigRead::Error || read == ConfigRead::Invalid) {
        ESP_LOGW(kTag, "Existing SD configuration unreadable or invalid; retaining files");
        return ConfigSyncResult::Failed;
    }
    if (read == ConfigRead::Found) {
        if (!validate_existing(previous)) {
            ESP_LOGW(kTag, "Existing SD config belongs to another format or is invalid; retaining files");
            return ConfigSyncResult::Failed;
        }
        if (!recovered && previous == data)
            return access.Close() ? ConfigSyncResult::Unchanged : ConfigSyncResult::Failed;
    }
    std::string().swap(previous);
    const bool ok = write_config_locked(data);
    return access.Close() && ok ? ConfigSyncResult::Updated : ConfigSyncResult::Failed;
}
} // namespace stackchan::sd
