/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "board/hal_bridge.h"
#include "drivers/PY32IOExpander_Class/PY32IOExpander_Class.hpp"
#include "utils/rgb_led_controller.h"
#include <mooncake_log.h>
#include <memory>
#include <esp_random.h>

static const std::string_view _tag = "HAL-IOE";

static std::unique_ptr<m5::PY32IOExpander_Class> _io_expander;

static stackchan::led::Controller _rgb_controller;

void Hal::io_expander_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus        = hal_bridge::board_get_i2c_bus();
    _io_expander        = std::make_unique<m5::PY32IOExpander_Class>(i2c_bus);
    uint32_t start_tick = GetHAL().millis();

    // PY32 IO Expander may boot slowly, wait for it
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));

        if (GetHAL().millis() - start_tick > 1200) {
            mclog::tagError(_tag, "init timeout");
            _io_expander.reset();
            break;
        }

        if (_io_expander->begin()) {
            break;
        }
        mclog::tagInfo(_tag, "init failed, retrying...");
    }

    if (_io_expander) {
        // VM EN
        _io_expander->setDirection(0, true);  // Output
        _io_expander->setPullMode(0, true);   // Pull-up
        GetHAL().setServoPowerEnabled(true);
        vTaskDelay(pdMS_TO_TICKS(200));

        // RGB
        _io_expander->setDirection(13, true);   // Output
        _io_expander->setPullMode(13, true);    // Pull-up
        _io_expander->setDriveMode(13, false);  // Push-pull
        _io_expander->setLedCount(12);
        vTaskDelay(pdMS_TO_TICKS(200));
        GetHAL().showRgbColor(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        GetHAL().showRgbColor(0, 0, 0);

        mclog::tagInfo(_tag, "init done");
    }
}

void Hal::setServoPowerEnabled(bool enabled)
{
    if (!_io_expander) {
        return;
    }
    _io_expander->digitalWrite(0, enabled ? true : false);
}

void Hal::setRgbColor(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!_io_expander) return;
    _rgb_controller.SetBase(index, {r, g, b});
}

void Hal::setConversationRgbColor(uint8_t r, uint8_t g, uint8_t b)
{
    if (!_io_expander) return;
    _rgb_controller.SetConversation({r, g, b});
    refreshRgb();
}

void Hal::setConversationRandomRgb()
{
    if (!_io_expander) return;
    _rgb_controller.SetConversationRandom();
    updateRandomRgb(false, false);
}

bool Hal::startRandomRgbTest()
{
    if (!_io_expander) return false;
    _rgb_controller.SetTestRandom(true);
    updateRandomRgb(true, false);
    return true;
}

void Hal::cancelRandomRgb()
{
    if (!_io_expander) return;
    _rgb_controller.CancelRandom();
    refreshRgb();
}

void Hal::updateRandomRgb(bool test_allowed, bool shutdown)
{
    if (!_io_expander) return;
    if (_rgb_controller.TickRandom(millis(), test_allowed, shutdown, [] { return esp_random(); }))
        refreshRgb();
}

void Hal::releaseConversationRgbColor()
{
    if (!_io_expander) return;
    _rgb_controller.ReleaseConversation();
    refreshRgb();
}

void Hal::setSleepRgbBlackout(bool enabled)
{
    if (!_io_expander) return;
    _rgb_controller.SetSleepBlackout(enabled);
    refreshRgb();
}

void Hal::refreshRgb()
{
    if (!_io_expander) return;
    _rgb_controller.Flush(
        [](unsigned index, uint8_t r, uint8_t g, uint8_t b) {
            _io_expander->setLedColor(index, r, g, b);
        },
        []() { _io_expander->refreshLeds(); },
        [](const stackchan::led::Controller::Frame& frame) {
            std::array<uint8_t, stackchan::led::Controller::kCount * 2> data{};
            for (unsigned i = 0; i < frame.size(); ++i) {
                const auto c = frame[i];
                const uint16_t rgb565 = ((c.r & 0xf8) << 8) | ((c.g & 0xfc) << 3) | (c.b >> 3);
                data[i * 2] = static_cast<uint8_t>(rgb565);
                data[i * 2 + 1] = static_cast<uint8_t>(rgb565 >> 8);
            }
            _io_expander->setLedData(data.data(), data.size());
        });
}

void Hal::showRgbColor(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 12; i++) {
        setRgbColor(i, r, g, b);
    }
    refreshRgb();
}
