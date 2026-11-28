/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <memory>
#include <atomic>
#include <esp_heap_caps.h>
#include <mooncake_log.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>
#include "board/hal_bridge.h"
#include "stackchan/storage/nvs_snapshot.h"
#include "stackchan/storage/factory_reset.h"
#include "stackchan/port_stage1/event_log.h"
#include "stackchan/config/server_boot_policy.h"
#include <stackchan/config/low_battery.h>
#include "stackchan/speech_mouth.h"
#include "stackchan/wifi/wifi_web.h"

static std::unique_ptr<Hal> _hal_instance;
static const std::string_view _tag = "HAL";

// An NVS commit, not a timer, requests the SD mirror. The backup worker
// exists only while a committed change is being copied to the SD card.
static std::atomic<uint32_t> _sd_nvs_revision{0};
static std::atomic<uint32_t> _sd_saved_revision{0};
static std::atomic<int64_t> _sd_retry_after_us{0};
static std::atomic<bool> _sd_backup_running{false};
static std::atomic<bool> _sd_backup_ready{false};
static void _schedule_sd_backup_if_safe();

extern "C" void stackchan_nvs_commit_observer(void)
{
    _sd_nvs_revision.fetch_add(1, std::memory_order_release);
    _sd_retry_after_us.store(0, std::memory_order_release);
    _schedule_sd_backup_if_safe();
}

extern "C" bool stackchan_sd_nvs_backup_pending(void)
{
    return _sd_nvs_revision.load(std::memory_order_acquire) !=
           _sd_saved_revision.load(std::memory_order_acquire);
}

Hal& GetHAL()
{
    if (!_hal_instance) {
        mclog::tagInfo(_tag, "creating hal instance");
        _hal_instance = std::make_unique<Hal>();
    }
    return *_hal_instance.get();
}

void Hal::init()
{
    mclog::tagInfo(_tag, "init");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(stackchan::storage::CompletePendingFactoryReset());

    xiaozhi_board_init();
    // The display/SD shared-bus controller is ready here; settings consumers
    // have not yet started. Restore only a truly empty NVS partition.
    if (stackchan::sd::RestoreNvsIfEmpty()) {
        mclog::tagInfo(_tag, "NVS recovered from SD; restarting to reload all settings");
        esp_restart();
        return;
    }
    stackchan::config::server_boot::PrepareStoredProtocol();
    xiaozhi_mcp_init();
    head_touch_init();
    io_expander_init();
    rtc_init();
    imu_init();
    servo_init();
    lvgl_init();
    _sd_backup_ready.store(true, std::memory_order_release);
    _schedule_sd_backup_if_safe();
}

/* -------------------------------------------------------------------------- */
/*                                   System                                   */
/* -------------------------------------------------------------------------- */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <system_info.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_mac.h>

void Hal::delay(std::uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

std::uint32_t Hal::millis()
{
    return esp_timer_get_time() / 1000;
}

void Hal::feedTheDog()
{
    vTaskDelay(1);
}

std::array<uint8_t, 6> Hal::getFactoryMac()
{
    std::array<uint8_t, 6> mac;
    esp_efuse_mac_get_default(mac.data());
    return mac;
}

std::string Hal::getFactoryMacString(std::string divider)
{
    auto mac = getFactoryMac();
    return fmt::format("{:02X}{}{:02X}{}{:02X}{}{:02X}{}{:02X}{}{:02X}", mac[0], divider, mac[1], divider, mac[2],
                       divider, mac[3], divider, mac[4], divider, mac[5]);
}

void Hal::reboot()
{
    esp_restart();
}

static void _confirm_ota_image_if_stable()
{
    constexpr uint32_t ota_confirm_delay_ms = 20000;
    static bool ota_confirm_checked         = false;
    if (ota_confirm_checked || GetHAL().millis() < ota_confirm_delay_ms) {
        return;
    }
    ota_confirm_checked = true;

    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running == nullptr) {
        mclog::tagError(_tag, "failed to get running partition for ota confirmation");
        return;
    }

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK) {
        mclog::tagError(_tag, "failed to get ota state for partition: {}", running->label);
        return;
    }

    mclog::tagInfo(_tag, "ota confirm check: partition={}, state={}", running->label, static_cast<int>(ota_state));
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        mclog::tagInfo(_tag, "ota image is stable, marking current app valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void Hal::updateHeapStatusLog()
{
    _confirm_ota_image_if_stable();

    static uint32_t last_log_tick = 0;
    if (millis() - last_log_tick < 10000) {
        return;
    }
    last_log_tick = millis();
    SystemInfo::PrintHeapStats();
}

/* -------------------------------------------------------------------------- */
/*                                   Xiaozhi                                  */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"
#include <stackchan/stackchan.h>
#include <apps/common/common.h>
#include <assets/assets.h>

void Hal::xiaozhi_board_init()
{
    mclog::tagInfo(_tag, "xiaozhi board init");

    hal_bridge::xiaozhi_board_init();
}

// A successful NVS commit triggers a snapshot. Do not reserve a permanent
// task stack, and never run optional SD work in the committing thread.
static void _sd_backup_task(void*)
{
    const uint32_t revision = _sd_nvs_revision.load(std::memory_order_acquire);
    const bool success = stackchan::sd::BackupNvsIfChanged();
    if (success) {
        _sd_saved_revision.store(revision, std::memory_order_release);
    } else {
        // A missing SD must not cause repeated task creation or starving audio.
        _sd_retry_after_us.store(esp_timer_get_time() + 60000000LL, std::memory_order_release);
    }
    _sd_backup_running.store(false, std::memory_order_release);
    vTaskDeleteWithCaps(nullptr);
}

static void _schedule_sd_backup_if_safe()
{
    if (stackchan::low_battery::Active() || !_sd_backup_ready.load(std::memory_order_acquire) ||
        _sd_nvs_revision.load(std::memory_order_acquire) ==
            _sd_saved_revision.load(std::memory_order_acquire) ||
        _sd_backup_running.load(std::memory_order_acquire) ||
        (hal_bridge::is_xiaozhi_ready() && !hal_bridge::is_xiaozhi_idle())) {
        return;
    }
    const int64_t retry_at = _sd_retry_after_us.load(std::memory_order_acquire);
    if (retry_at && esp_timer_get_time() < retry_at) return;

    // esp_flash_dispatcher runs cache-disabling NVS operations on its own
    // internal stack, so this short-lived worker can keep its stack in PSRAM.
    // Retain enough internal heap for the SD driver, Wi-Fi, audio and DMA.
    constexpr size_t kMinInternalFree = 28 * 1024;
    constexpr size_t kMinLargestBlock = 6 * 1024;
    constexpr size_t kMinPsramFree = 32 * 1024;
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (free_bytes < kMinInternalFree || largest < kMinLargestBlock ||
        psram < kMinPsramFree) return;

    bool expected = false;
    if (!_sd_backup_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }
    TaskHandle_t task = nullptr;
    if (xTaskCreateWithCaps(_sd_backup_task, "sd_nvs_backup", 6144, nullptr, 1, &task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        _sd_backup_running.store(false, std::memory_order_release);
        _sd_retry_after_us.store(esp_timer_get_time() + 60000000LL, std::memory_order_release);
        mclog::tagError(_tag, "SD NVS backup task creation failed");
    } else {
        mclog::tagInfo(_tag, "SD NVS backup task started with PSRAM stack");
    }
}

static void _stackchan_update_task(void* param)
{
    bool is_setup_done = false;
    uint32_t last_sd_backup_check = GetHAL().millis();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));
        const uint32_t now = GetHAL().millis();
        if (now - last_sd_backup_check >= 1000) {
            last_sd_backup_check = now;
            stackchan::stage1::event_log::Poll();
            // Retry a pending NVS-save event outside the LVGL lock. No
            // periodic SD snapshot is made without a committed NVS change.
            if (_sd_nvs_revision.load(std::memory_order_acquire) !=
                _sd_saved_revision.load(std::memory_order_acquire)) {
                _schedule_sd_backup_if_safe();
            }
        }

        // Apply a saved Wi-Fi change after the HTTP response and only while
        // conversation processing is idle; reuse this task (no new stack).
        if (!stackchan::low_battery::Active()) {
            stackchan::wifi::web::ApplyPendingIfIdle();
            tools::update_reminders();
        }

        // Keep the conversation pacing delay OUTSIDE the LVGL lock so
        // subtitles and audio-related UI callbacks are not blocked by it.
        // Retain the existing task and audio/listening pacing. Speech
        // gets an approximately 30ms avatar update instead of 120ms.
        if (!hal_bridge::is_xiaozhi_idle()) {
            vTaskDelay(pdMS_TO_TICKS(stackchan::speech_mouth::Playing() ? 10 : 100));
        }

        LvglLockGuard lock;
        GetStackChan().update();
        const bool shutdown = stackchan::low_battery::Active();
        GetHAL().updateRandomRgb(!shutdown && stackchan::stage1::CanRunRgbTest(), shutdown);
        if (shutdown) continue;

        if (!hal_bridge::is_xiaozhi_ready()) {
            continue;
        }

        // Direct boot does not run the OTA version check. Keep bootloader
        // rollback confirmation on the existing stable-uptime path.
        _confirm_ota_image_if_stable();
        if (!is_setup_done) {
            // Setup when xiaozhi ready
            GetHAL().startSntp();
            view::create_home_indicator([]() { GetHAL().requestWarmReboot(0); }, 0x81DBBD, 0x134233);
            view::create_status_bar(0x81DBBD, 0x134233);
            is_setup_done = true;
        }

        view::update_home_indicator();
        view::update_status_bar();
    }
}

void Hal::startXiaozhi()
{
    mclog::tagInfo(_tag, "start xiaozhi");

    auto& motion = GetStackChan().motion();
    motion.setAutoAngleSyncEnabled(true);
    motion.setAutoTorqueReleaseEnabled(true);

    // Setup reminder handler
    tools::on_reminder_triggered().clear();
    tools::on_reminder_triggered().connect([](int id, std::string_view msg) {
        mclog::tagInfo(_tag, "reminder triggered: id: {}, msg: {}", id, msg);
        {
            LvglLockGuard lock;
            auto& avatar = GetStackChan().avatar();
            avatar.addDecorator(std::make_unique<view::ReminderView>(lv_screen_active(), msg));
        }
        hal_bridge::app_play_sound(OGG_NEW_NOTIFICATION);
    });

    // Start stackchan update task
    xTaskCreatePinnedToCore(_stackchan_update_task, "stackchan", 4096, NULL, 3, NULL, 1);

    hal_bridge::start_xiaozhi_app();
}

XiaozhiConfig_t Hal::getXiaozhiConfig()
{
    auto bridge_config = hal_bridge::get_xiaozhi_config();
    return XiaozhiConfig_t{
        .idleRandomMovementLevel   = bridge_config.idleRandomMovementLevel,
        .startAiAgentOnBoot        = bridge_config.startAiAgentOnBoot,
    };
}

void Hal::setXiaozhiConfig(XiaozhiConfig_t config)
{
    hal_bridge::set_xiaozhi_config({
        .idleRandomMovementLevel   = config.idleRandomMovementLevel,
        .startAiAgentOnBoot        = config.startAiAgentOnBoot,
    });
}

uint8_t Hal::getBatteryLevel()
{
    return hal_bridge::board_get_battery_level();
}

bool Hal::isBatteryCharging()
{
    return hal_bridge::board_is_battery_charging();
}

void Hal::factoryReset()
{
    mclog::tagInfo(_tag, "start factory reset");
    _sd_backup_ready.store(false, std::memory_order_release);
    ESP_ERROR_CHECK(stackchan::storage::RequestFactoryReset());
    reboot();
}

/* -------------------------------------------------------------------------- */
/*                                   Display                                  */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"

void Hal::lvglLock()
{
    hal_bridge::disply_lvgl_lock();
}

void Hal::lvglUnlock()
{
    hal_bridge::disply_lvgl_unlock();
}

void Hal::setBackLightBrightness(uint8_t brightness, bool permanent)
{
    hal_bridge::board_set_backlight_brightness(brightness, permanent);
}

uint8_t Hal::getBackLightBrightness()
{
    return hal_bridge::board_get_backlight_brightness();
}

void Hal::setSpeakerVolume(uint8_t volume, bool permanent)
{
    hal_bridge::board_set_speaker_volume(volume, permanent);
}

uint8_t Hal::getSpeakerVolume()
{
    return hal_bridge::board_get_speaker_volume();
}

/* -------------------------------------------------------------------------- */
/*                                    Lvgl                                    */
/* -------------------------------------------------------------------------- */
#include "board/hal_bridge.h"
#include <stackchan/stackchan.h>

static void lvgl_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    hal_bridge::lock();
    auto& bridge_data = hal_bridge::get_data();

    // mclog::tagInfo(_tag, "touchpoint: {}, x: {}, y: {}", bridge_data.touchPoint.num, bridge_data.touchPoint.x,
    //                bridge_data.touchPoint.y);

    if (bridge_data.touchPoint.num == 0) {
        data->state = LV_INDEV_STATE_RELEASED;
    } else {
        data->state   = LV_INDEV_STATE_PRESSED;
        data->point.x = bridge_data.touchPoint.x;
        data->point.y = bridge_data.touchPoint.y;
    }

    hal_bridge::unlock();
}

void Hal::lvgl_init()
{
    mclog::tagInfo(_tag, "lvgl init");

    hal_bridge::disply_lvgl_lock();

    mclog::tagInfo(_tag, "create lvgl touchpad indev");
    lvTouchpad = lv_indev_create();
    lv_indev_set_type(lvTouchpad, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(lvTouchpad, lvgl_read_cb);
    lv_indev_set_group(lvTouchpad, lv_group_get_default());
    lv_indev_set_display(lvTouchpad, hal_bridge::display_get_lvgl_display());

    hal_bridge::disply_lvgl_unlock();
}

/* -------------------------------------------------------------------------- */
/*                                 Warm Reboot                                */
/* -------------------------------------------------------------------------- */
#include <settings.h>
#include <string_view>

static std::string_view _warm_boot_nvs_ns  = "warm_boot";
static std::string_view _warm_boot_nvs_key = "app_index";

void Hal::requestWarmReboot(int appIndex)
{
    mclog::tagInfo(_tag, "warm reboot request to app index: {}", appIndex);

    {
        Settings settings(_warm_boot_nvs_ns.data(), true);
        settings.SetInt(_warm_boot_nvs_key.data(), appIndex);
    }

    delay(100);
    esp_restart();
}

int Hal::getWarmRebootTarget()
{
    Settings settings(_warm_boot_nvs_ns.data(), false);
    return settings.GetInt(_warm_boot_nvs_key.data(), -1);
}

void Hal::clearWarmRebootRequest()
{
    mclog::tagInfo(_tag, "clear warm reboot request");

    Settings settings(_warm_boot_nvs_ns.data(), true);
    settings.SetInt(_warm_boot_nvs_key.data(), -1);
}
