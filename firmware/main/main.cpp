/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <mooncake_log.h>
#include <mooncake.h>
#include <apps/apps.h>
#include <hal/hal.h>
#include <hal/hal_toggles.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_flash_dispatcher.h>
#include <stackchan/port_stage1/event_log.h>
#include <cstdlib>

using namespace mooncake;
using namespace smooth_ui_toolkit;

extern "C" void app_main(void)
{
    // Setup logger
    mclog::set_level(mclog::level_info);
    mclog::set_time_format(mclog::time_format_unix_milliseconds);
    stackchan::stage1::event_log::Start();

    // Run cache-disabling flash operations on the smallest dedicated internal
    // stack while keeping HTTP and other eligible task stacks in PSRAM.
    esp_flash_dispatcher_config_t flash_config{};
    flash_config.task_stack_size = 2048;
    flash_config.task_priority = 10;
    flash_config.task_core_id = tskNO_AFFINITY;
    flash_config.queue_size = 1; // retained by the component for compatibility
    ESP_ERROR_CHECK(esp_flash_dispatcher_init(&flash_config));

    // Put dynamically built JSON trees and serialized JSON in PSRAM, including
    // Web settings and protocol payloads. Initialize hooks once before HAL
    // starts any tasks that use cJSON. Match cJSON's free hook to the default
    // heap allocator so both PSRAM and fallback allocations are released.
    static cJSON_Hooks json_hooks{
        [](size_t size) -> void* {
            void* memory = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            return memory != nullptr ? memory : std::malloc(size);
        },
        [](void* memory) { std::free(memory); },
    };
    cJSON_InitHooks(&json_hooks);

    // HAL init
    GetHAL().init();

    // Setup ui hal
    ui_hal::on_delay([](uint32_t ms) { GetHAL().delay(ms); });
    ui_hal::on_get_tick([]() { return GetHAL().millis(); });

    const bool skip_mooncake = GetHAL().getXiaozhiConfig().startAiAgentOnBoot && GetHAL().getWarmRebootTarget() < 0;

    if (!skip_mooncake) {
        // Install apps
        GetMooncake().installApp(std::make_unique<AppLauncher>());
        hal_toggles::register_app_id(hal_toggles::App::AiAgent,
                                     GetMooncake().installApp(std::make_unique<AppAiAgent>()));
        hal_toggles::register_setup_app_id(GetMooncake().installApp(std::make_unique<AppSetup>()));

        // Main loop
        while (1) {
            GetHAL().feedTheDog();
            GetHAL().updateHeapStatusLog();

            GetMooncake().update();

            if (GetHAL().isXiaozhiStartRequested()) {
                break;
            }
        }

        // Uninstall all apps and destroy mooncake
        GetMooncake().uninstallAllApps();
        DestroyMooncake();
    }

    // Start xiaozhi, never returns
    GetHAL().startXiaozhi();
}
