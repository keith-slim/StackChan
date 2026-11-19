/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <stackchan/port_stage1/port_stage1.h>
#include "drivers/Si12T/Si12T.h"
#include "board/hal_bridge.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

static const std::string_view _tag = "HAL-HeadTouch";

// 触摸状态
enum class TouchState { IDLE, TOUCHED, SWIPING };

// 配置参数
struct TouchConfig {
    uint8_t touch_threshold = 1;
    int16_t swipe_threshold = 40;  // 使用百分比，范围-100到100
};

// 触摸数据
struct TouchData {
    uint8_t intensity[3];
    uint32_t timestamp;

    // 计算位置（返回-100到100的整数）
    int16_t get_position() const
    {
        uint16_t total = intensity[0] + intensity[1] + intensity[2];
        if (total == 0) return 0;

        int32_t weighted = intensity[0] * (-100) + intensity[1] * 0 + intensity[2] * 100;
        return static_cast<int16_t>(weighted / total);
    }

    uint8_t get_max_intensity() const
    {
        uint8_t max_val = intensity[0];
        if (intensity[1] > max_val) max_val = intensity[1];
        if (intensity[2] > max_val) max_val = intensity[2];
        return max_val;
    }

    bool is_touched() const
    {
        return get_max_intensity() >= 1;
    }
};

// 手势识别器类
class GestureRecognizer {
public:
    GestureRecognizer() : current_state(TouchState::IDLE), initial_position(0)
    {
    }

    // 更新状态机，返回识别到的手势
    HeadPetGesture update(const TouchData& data)
    {
        HeadPetGesture gesture = HeadPetGesture::None;

        switch (current_state) {
            case TouchState::IDLE:
                if (data.is_touched()) {
                    current_state    = TouchState::TOUCHED;
                    initial_position = data.get_position();
                    gesture          = HeadPetGesture::Press;
                    // mclog::tagInfo(_tag, "Touch detected at position: {}", initial_position);
                }
                break;

            case TouchState::TOUCHED:
                if (!data.is_touched()) {
                    current_state = TouchState::IDLE;
                    gesture       = HeadPetGesture::Release;
                } else {
                    // Check for swipe
                    int16_t current_pos = data.get_position();
                    int16_t delta       = current_pos - initial_position;

                    if (delta > config.swipe_threshold) {
                        current_state = TouchState::SWIPING;
                        gesture       = HeadPetGesture::SwipeForward;
                        // mclog::tagInfo(_tag, "Swipe forward detected, delta: {}", delta);
                    } else if (delta < -config.swipe_threshold) {
                        current_state = TouchState::SWIPING;
                        gesture       = HeadPetGesture::SwipeBackward;
                        // mclog::tagInfo(_tag, "Swipe backward detected, delta: {}", delta);
                    }
                }
                break;

            case TouchState::SWIPING:
                if (!data.is_touched()) {
                    current_state = TouchState::IDLE;
                    gesture       = HeadPetGesture::Release;
                }
                break;
        }

        return gesture;
    }

    void set_config(const TouchConfig& cfg)
    {
        config = cfg;
    }

private:
    TouchConfig config;
    TouchState current_state;
    int16_t initial_position;
};

static void _head_touch_update_task(void* param)
{
    mclog::tagInfo(_tag, "start update task");

    si12t_handle_t si12t = (si12t_handle_t)param;
    uint8_t touch_result = 0;
    TouchData data;

    GestureRecognizer recognizer;
    HeadPetGesture gesture;
    // A one-sample capacitive spike must not wake the sleeping avatar.
    // Only the sleep path is debounced; awake pet gestures are unchanged.
    uint8_t sleep_touch_samples = 0;
    bool suppress_until_release = false;
    // Diagnostic-only snapshot of a sleep touch candidate. SI12T OUTPUT1 is
    // a packed 2-bit-per-channel result, not an analog capacitance reading.
    uint8_t sleep_touch_raw[3] = {};
    uint32_t sleep_touch_started = 0;
    bool sleep_diag_read_failed = false;

    vTaskDelay(pdMS_TO_TICKS(200));

    while (1) {
        // Read data
        const esp_err_t read_result = si12t_read_touch_result(si12t, &touch_result);
        if (stackchan::stage1::IsSleepWakeEligible() && read_result != ESP_OK) {
            if (!sleep_diag_read_failed)
                ESP_LOGW("HEAD_TOUCH_DIAG", "sleep read_error=%s (touch data may be stale)",
                         esp_err_to_name(read_result));
            sleep_diag_read_failed = true;
        } else if (read_result == ESP_OK) {
            sleep_diag_read_failed = false;
        }
        si12t_parse_touch_result_to(touch_result, data.intensity);
        data.timestamp = xTaskGetTickCount();

        // Update recognizer even while sleeping, so an old press/release is
        // never replayed as a normal pet gesture after waking.
        gesture = recognizer.update(data);
        const bool touched = data.is_touched();
        if (stackchan::stage1::IsSleepWakeEligible()) {
            suppress_until_release = true;
            if (touched) {
                if (sleep_touch_samples == 0) {
                    sleep_touch_started = data.timestamp;
                    ESP_LOGI("HEAD_TOUCH_DIAG",
                             "sleep touch_start raw=0x%02x ch=%u,%u,%u threshold=1 sensitivity=LOW/3",
                             touch_result, data.intensity[0], data.intensity[1], data.intensity[2]);
                }
                if (sleep_touch_samples < 3) {
                    sleep_touch_raw[sleep_touch_samples] = touch_result;
                    ++sleep_touch_samples;
                }
                if (sleep_touch_samples == 3) {
                    // Diagnostic snapshot only: keep the existing wake rule.
                    const bool accepted = stackchan::stage1::RequestWakeTouch("head_pad");
                    ESP_LOGI("HEAD_TOUCH_DIAG",
                             "sleep wake_check accepted=%d samples=3 duration_ticks=%lu raw=0x%02x,0x%02x,0x%02x ch=%u,%u,%u",
                             accepted, static_cast<unsigned long>(data.timestamp - sleep_touch_started),
                             sleep_touch_raw[0], sleep_touch_raw[1], sleep_touch_raw[2],
                             data.intensity[0], data.intensity[1], data.intensity[2]);
                    ++sleep_touch_samples; // enqueue only once per hold
                }
            } else {
                if (sleep_touch_samples != 0) {
                    ESP_LOGI("HEAD_TOUCH_DIAG",
                             "sleep touch_end samples=%u duration_ticks=%lu raw=0x%02x ch=%u,%u,%u",
                             sleep_touch_samples, static_cast<unsigned long>(data.timestamp - sleep_touch_started),
                             touch_result, data.intensity[0], data.intensity[1], data.intensity[2]);
                }
                sleep_touch_samples = 0;
            }
        } else if (suppress_until_release) {
            if (sleep_touch_samples != 0) {
                ESP_LOGI("HEAD_TOUCH_DIAG",
                         "sleep phase_exit samples=%u duration_ticks=%lu raw=0x%02x touched=%d",
                         sleep_touch_samples, static_cast<unsigned long>(data.timestamp - sleep_touch_started),
                         touch_result, touched);
                sleep_touch_samples = 0;
            }
            // Do not forward the release of a sleep/wake touch to the normal
            // pet handler. That release could reset the new wake scene.
            if (!touched) {
                suppress_until_release = false;
                sleep_touch_samples = 0;
            }
        } else if (gesture != HeadPetGesture::None &&
                   !stackchan::stage1::IsWakeSequenceActive()) {
            stackchan::stage1::NotifyActivity("head_pad");
            GetHAL().onHeadPetGesture.emit(gesture);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void Hal::head_touch_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus = hal_bridge::board_get_i2c_bus();

    si12t_config_t si12t_cfg = {
        .i2c_bus  = i2c_bus,
        .dev_addr = SI12T_GND_ADDRESS,
    };
    static si12t_handle_t si12t;
    si12t_init(&si12t_cfg, &si12t);
    si12t_setup(si12t, SI12T_TYPE_LOW, SI12T_SENSITIVITY_LEVEL_3);

    // xTaskCreateWithCaps(_head_touch_update_task, "headtouch", 1024 * 6, si12t, 2, NULL, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(_head_touch_update_task, "headtouch", 1024 * 6, si12t, 2, NULL, 1,
                                    MALLOC_CAP_SPIRAM);
}
