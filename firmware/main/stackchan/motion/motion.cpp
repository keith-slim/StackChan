/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "motion.h"
#include "motion_math.h"
#include "servo_executor.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <esp_log.h>

using namespace uitk;
using namespace stackchan::motion;

void Motion::init()
{
    _yaw_servo->init();
    _pitch_servo->init();
    _worker_stop.store(false);
    executor::done = xSemaphoreCreateBinaryStatic(&executor::done_storage);
    _worker_running.store(true);
    if (!executor::done || xTaskCreateWithCaps(UpdateWorker, "servo_update", 4096, this, 4, nullptr,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        _worker_running.store(false);
        ESP_LOGW("Motion", "Servo PSRAM task allocation failed; using existing update path");
    } else {
        // Initialization has finished before publishing the worker to callers.
        while (!executor::owner.load(std::memory_order_acquire)) vTaskDelay(1);
    }
}

Motion::~Motion()
{
    std::lock_guard<std::mutex> lock(executor::submit_mutex);
    _worker_stop.store(true);
    if (auto task = executor::owner.load(std::memory_order_acquire)) xTaskNotifyGive(task);
    while (_worker_running.load()) vTaskDelay(1);
}

void Motion::UpdateWorker(void* arg)
{
    auto* self = static_cast<Motion*>(arg);
    executor::owner.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    const TickType_t period = pdMS_TO_TICKS(20) ? pdMS_TO_TICKS(20) : 1;
    TickType_t last = xTaskGetTickCount() - period;
    int64_t report_at = esp_timer_get_time();
    uint32_t cycles = 0, overruns = 0, max_us = 0, max_gap_ms = 0;
    TickType_t previous_update = last;
    while (!self->_worker_stop.load()) {
        self->_yaw_servo->servicePriorityStop();
        executor::service();
        const TickType_t now = xTaskGetTickCount();
        const TickType_t elapsed = now - last;
        if (elapsed >= period) {
            const uint32_t gap_ms = (now - previous_update) * portTICK_PERIOD_MS;
            previous_update = now;
            if (gap_ms > max_gap_ms) max_gap_ms = gap_ms;
            last += (elapsed / period) * period;
            const int64_t started = esp_timer_get_time();
            self->updateServos();
            const uint32_t us = esp_timer_get_time() - started;
            ++cycles;
            if (us > 20000) ++overruns;
            if (us > max_us) max_us = us;
            if (started - report_at >= 10000000) {
                ESP_LOGI("Motion", "[SERVO_CYCLE] cycles=%lu overruns=%lu max_us=%lu max_gap_ms=%lu",
                         (unsigned long)cycles, (unsigned long)overruns,
                         (unsigned long)max_us, (unsigned long)max_gap_ms);
                ESP_LOGI("Motion", "[SERVO_DISPATCH] requests=%lu wait_max_us=%lu run_max_us=%lu",
                         (unsigned long)executor::request_count, (unsigned long)executor::wait_max_us,
                         (unsigned long)executor::run_max_us);
                executor::request_count = executor::wait_max_us = executor::run_max_us = 0;
                cycles = overruns = max_us = max_gap_ms = 0;
                report_at = started;
            }
        }
        const TickType_t spent = xTaskGetTickCount() - last;
        if (spent < period) ulTaskNotifyTake(pdTRUE, period - spent);
    }
    executor::owner.store(nullptr, std::memory_order_release);
    self->_worker_running.store(false);
    vTaskDeleteWithCaps(nullptr);
}

void Motion::update()
{
    // Preserve the original path if task allocation fails.
    if (!_worker_running.load()) updateServos();
}

void Motion::updateServos()
{
    _yaw_servo->update();
    executor::service(); // dispatch before lower-priority pitch monitoring
    _pitch_servo->update();
}

Servo& Motion::yawServo()
{
    return *_yaw_servo;
}

Servo& Motion::pitchServo()
{
    return *_pitch_servo;
}

void Motion::moveYaw(int angle)
{
    _yaw_servo->move(angle);
}

void Motion::moveYawWithSpeed(int angle, int speed)
{
    _yaw_servo->moveWithSpeed(angle, speed);
}

void Motion::movePitch(int angle)
{
    _pitch_servo->move(angle);
}

void Motion::movePitchWithSpeed(int angle, int speed)
{
    _pitch_servo->moveWithSpeed(angle, speed);
}

void Motion::move(int yawAngle, int pitchAngle)
{
    _yaw_servo->move(yawAngle);
    _pitch_servo->move(pitchAngle);
}

void Motion::moveWithSpeed(int yawAngle, int pitchAngle, int speed)
{
    _yaw_servo->moveWithSpeed(yawAngle, speed);
    _pitch_servo->moveWithSpeed(pitchAngle, speed);
}

void Motion::trackWithSpeed(int yawAngle, int pitchAngle, int speed)
{
    _yaw_servo->trackWithSpeed(yawAngle, speed);
    _pitch_servo->trackWithSpeed(pitchAngle, speed);
}

void Motion::moveWithDuration(int yawAngle, int pitchAngle, uint32_t duration_ms, uint32_t now)
{
    _yaw_servo->moveWithDuration(yawAngle, duration_ms, now);
    _pitch_servo->moveWithDuration(pitchAngle, duration_ms, now);
}

void Motion::cancelTimedMove()
{
    _yaw_servo->cancelTimedMove();
    _pitch_servo->cancelTimedMove();
}

void Motion::moveRelativeFrom(const uitk::Vector2i& origin, int yawDelta, int pitchDelta, int speed)
{
    const auto yaw_limit = _yaw_servo->getAngleLimit();
    const auto pitch_limit = _pitch_servo->getAngleLimit();
    moveWithSpeed(
        clampRelativeAngle(origin.x, yawDelta, yaw_limit.x, yaw_limit.y),
        clampRelativeAngle(origin.y, pitchDelta, pitch_limit.x, pitch_limit.y),
        speed);
}

void Motion::goHome(int speed)
{
    _yaw_servo->moveWithSpeed(0, speed);
    _pitch_servo->moveWithSpeed(0, speed);
}

void Motion::stop()
{
    _yaw_servo->move(_yaw_servo->getCurrentAngle());
    _pitch_servo->move(_pitch_servo->getCurrentAngle());
}

void Motion::lookAtNormalized(float x, float y, int speed)
{
    auto angles = calculateNormalizedLookAngles(
        x, y, _yaw_servo->getAngleLimit().x, _yaw_servo->getAngleLimit().y, _pitch_servo->getAngleLimit().x, _pitch_servo->getAngleLimit().y);
    moveWithSpeed(angles.yaw, angles.pitch, speed);
}

void Motion::lookAtPoint(float x, float y, float z, int speed)
{
    auto angles = calculatePointLookAngles(x, y, z);
    moveWithSpeed(angles.yaw, angles.pitch, speed);
}

bool Motion::isMoving()
{
    return _yaw_servo->isMoving() || _pitch_servo->isMoving();
}

int Motion::getCurrentYawAngle()
{
    return _yaw_servo->getCurrentAngle();
}

int Motion::getCurrentPitchAngle()
{
    return _pitch_servo->getCurrentAngle();
}

uitk::Vector2i Motion::getCurrentAngles()
{
    return uitk::Vector2i(_yaw_servo->getCurrentAngle(), _pitch_servo->getCurrentAngle());
}

void Motion::setTorqueEnabled(bool enabled)
{
    _yaw_servo->setTorqueEnabled(enabled);
    _pitch_servo->setTorqueEnabled(enabled);
}

void Motion::setAutoTorqueReleaseEnabled(bool enabled)
{
    _yaw_servo->setAutoTorqueReleaseEnabled(enabled);
    _pitch_servo->setAutoTorqueReleaseEnabled(enabled);
}

void Motion::setAutoAngleSyncEnabled(bool enabled)
{
    _yaw_servo->setAutoAngleSyncEnabled(enabled);
    _pitch_servo->setAutoAngleSyncEnabled(enabled);
}

void Motion::setModifyLock(bool locked)
{
    _is_modify_locked = locked;
}

bool Motion::isModifyLocked()
{
    return _is_modify_locked;
}
