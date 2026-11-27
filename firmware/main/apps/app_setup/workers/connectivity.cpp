/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 * SPDX-License-Identifier: MIT
 */
#include "workers.h"
#include <hal/hal.h>
#include <wifi_manager.h>
#include <assets/lang_config.h>
#include <mooncake_log.h>

extern "C" void stackchan_nvs_commit_observer(void);

using namespace smooth_ui_toolkit::lvgl_cpp;
using namespace setup_workers;

WifiSetupWorker::WifiSetupWorker()
{
    _panel = std::make_unique<Container>(lv_screen_active());
    _panel->setBgColor(lv_color_hex(0xEDF4FF));
    _panel->align(LV_ALIGN_CENTER, 0, 0);
    _panel->setSize(320, 240);
    _panel->setBorderWidth(0);
    _panel->setRadius(0);

    _info = std::make_unique<Label>(_panel->get());
    _info->setWidth(300);
    _info->setTextFont(&lv_font_montserrat_16);
    _info->setTextColor(lv_color_hex(0x26206A));
    _info->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _info->align(LV_ALIGN_CENTER, 0, -30);
    _info->setText("Starting Wi-Fi setup...");

    _back = std::make_unique<Button>(_panel->get());
    apply_button_common_style(*_back);
    _back->align(LV_ALIGN_BOTTOM_MID, 0, -12);
    _back->setSize(140, 44);
    _back->label().setText("Close AP");
    _back->onClick().connect([this]() { _close_requested = true; });
}

WifiSetupWorker::~WifiSetupWorker()
{
    stop_ap();
}

void WifiSetupWorker::stop_ap()
{
    if (!_provisioning) return;
    auto& wifi = WifiManager::GetInstance();
    wifi.StopConfigAp();
    network_diagnostics::finishWifiProvisioning();
    _provisioning = false;
    // The component commits saved credentials directly to NVS, outside
    // Settings. Notify the existing SD mirror after the portal is closed.
    stackchan_nvs_commit_observer();
}

void WifiSetupWorker::update()
{
    if (_close_requested) {
        stop_ap();
        _is_done = true;
        return;
    }
    auto& wifi = WifiManager::GetInstance();
    if (!_started) {
        _started = true;
        // Only this setup page owns the AP. Reuse the same driver handover
        // lock as the network diagnostic; no AI application or BLE is needed.
        network_diagnostics::prepareWifiProvisioning();
        _provisioning = true;
        wifi.SetEventCallback({});
        WifiManagerConfig config;
        config.ssid_prefix = "Xiaozhi";
        config.language = Lang::CODE;
        config.show_ota_config = false;
        config.show_sleep_config = false;
        if (!wifi.IsInitialized() && !wifi.Initialize(config)) {
            _info->setText("Wi-Fi setup failed.\nClose and try again.");
            stop_ap();
            return;
        }
        wifi.StartConfigAp();
        if (!wifi.IsConfigMode()) {
            _info->setText("Could not start AP.\nClose and try again.");
            stop_ap();
            return;
        }
        _info->setText(fmt::format("Connect to Wi-Fi:\n{}\n\nOpen in browser:\n{}\n\nNo password required.",
                                  wifi.GetApSsid(), wifi.GetApWebUrl()));
        mclog::tagInfo("WiFiSetup", "Local configuration AP started");
    } else if (_provisioning && !wifi.IsConfigMode()) {
        // The library's /exit handler stops the AP after sending its HTTP
        // response. Do not restart the AP or wait for a cloud binding.
        stop_ap();
        mclog::tagInfo("WiFiSetup", "Local configuration AP closed");
        _is_done = true;
    }
}
