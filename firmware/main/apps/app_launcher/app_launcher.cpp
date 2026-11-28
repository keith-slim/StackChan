/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_launcher.h"
#include <hal/hal.h>
#include <hal/hal_toggles.h>
#include <mooncake.h>
#include <mooncake_log.h>
#include <stackchan/stackchan.h>
#include <cstdint>

using namespace mooncake;

void AppLauncher::onLauncherCreate()
{
    mclog::tagInfo(getAppInfo().name, "on create");

    // 打开自己
    open();
}

void AppLauncher::onLauncherOpen()
{
    mclog::tagInfo(getAppInfo().name, "on open");

    LvglLockGuard lock;

    if (!_startup_checked && GetHAL().consumeAppSetupRequest()) {
        mclog::tagInfo(getAppInfo().name, "resume local Wi-Fi setup after connection failure");
        _startup_worker = std::make_unique<setup_workers::WifiSetupWorker>();
    } else if (!_startup_checked && !GetHAL().hasSavedWifi()) {
        mclog::tagInfo(getAppInfo().name, "no saved Wi-Fi, start startup worker");
        _startup_worker = std::make_unique<setup_workers::StartupWorker>();
    } else {
        create_launcher_view();
    }
}

void AppLauncher::onLauncherRunning()
{
    LvglLockGuard lock;

    if (_startup_worker) {
        _startup_worker->update();
        if (_startup_worker->isDone()) {
            _startup_worker.reset();
            _startup_checked = true;
            create_launcher_view();
        }
    } else {
        _view->update();
        screensaver_update();
    }

    GetStackChan().update();
}

void AppLauncher::onLauncherClose()
{
    mclog::tagInfo(getAppInfo().name, "on close");

    LvglLockGuard lock;

    _view.reset();
}

void AppLauncher::onLauncherDestroy()
{
    mclog::tagInfo(getAppInfo().name, "on close");
}

void AppLauncher::create_launcher_view()
{
    _view = std::make_unique<view::LauncherView>();
    hal_toggles::ToggleStates states;
    if (!hal_toggles::load_states(states)) {
        mclog::tagWarn(getAppInfo().name, "Unable to load menu preferences; showing all apps");
    }
    auto app_props       = getAppProps();
    int restore_app_id   = -1;
    const int warm_index = GetHAL().getWarmRebootTarget();
    if (warm_index >= 0) {
        // Persisted 0 and 6 identify AI and SETUP in the old seven-app
        // layout. Resolve through installed IDs, never the current list index.
        restore_app_id = hal_toggles::boot_focus_app_id(warm_index);
        GetHAL().clearWarmRebootRequest();
    }
    std::vector<mooncake::AppProps_t> filtered_props;
    for (const auto& props : app_props) {
        if (hal_toggles::is_app_enabled(props.appID, states)) {
            filtered_props.push_back(props);
        }
    }
    _view->init(filtered_props, restore_app_id);
    _view->onAppClicked = [&](int appID) {
        mclog::tagInfo(getAppInfo().name, "handle open app, app id: {}", appID);
        openApp(appID);
    };
}

void AppLauncher::screensaver_update()
{
    const uint32_t SCREENSAVER_TIMEOUT_MS = 30000;

    uint32_t idle_time = lv_display_get_inactive_time(NULL);
    if (idle_time >= SCREENSAVER_TIMEOUT_MS) {
        if (!_screensaver) {
            _screensaver = std::make_unique<view::Screensaver>();
            _screensaver->init();
        }
    } else if (_screensaver) {
        _screensaver.reset();
    }

    // Update in 30ms interval
    if (_screensaver && GetHAL().millis() - _screensaver_timecount > 30) {
        _screensaver_timecount = GetHAL().millis();
        _screensaver->update();
    }
}
