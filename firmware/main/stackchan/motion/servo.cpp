/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "servo.h"
#include <hal/hal.h>
#include <cmath>

using namespace uitk;

namespace stackchan::motion {

static SpringOptions_t _default_spring_options = {
    .stiffness = 170.0,
    .damping   = 26.0,

    .mass     = 1.0,
    .velocity = 0.0,

    .restSpeed = 0.1,
    .restDelta = 0.1,

    .duration       = 0.0,
    .bounce         = 0.0,
    .visualDuration = 0.0,
};

void Servo::init()
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    apply_default_spring_options();

    _angle_anim.teleport(getCurrentAngle());
    update();

    setTorqueEnabled(false);
}

void Servo::update()
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    // Keep update in at most 50Hz
    if (GetHAL().millis() - _last_tick < 20) {
        return;
    }
    _last_tick = GetHAL().millis();

    // Keep the existing low-level clamp/stall protection, but do not
    // start a new spring or read physical position for every timed setpoint.
    if (_timed_move.Active()) {
        const int angle = uitk::clamp(_timed_move.Sample(_last_tick), _angle_limit.x, _angle_limit.y);
        if (angle != static_cast<int>(_angle_anim.directValue())) {
            _angle_anim.teleport(angle);
            set_angle_impl(angle);
        }
        return;
    }

    // Rate-limited critically damped spring. Keep fractional position and
    // velocity between updates and between retargets; never snap to a distant goal.
    if (_tracking_target) {
        const int goal = uitk::clamp(_tracking_goal, _angle_limit.x, _angle_limit.y);
        // Integrate actual elapsed time in bounded steps. The acceleration
        // bound uses the existing spring damping time scale, not a second speed setting.
        uint32_t elapsed = _last_tick - _tracking_tick;
        _tracking_tick = _last_tick;
        const float rate = static_cast<float>(_tracking_rate);
        const float acceleration_limit = rate * _tracking_damping;
        while (elapsed && _tracking_target) {
            const uint32_t step = elapsed > 20 ? 20 : elapsed;
            elapsed -= step;
            const float dt = step * 0.001f;
            const float error = goal - _tracking_position;
            const float acceleration = uitk::clamp(
                _tracking_stiffness * error - _tracking_damping * _tracking_velocity,
                -acceleration_limit, acceleration_limit);
            _tracking_velocity = uitk::clamp(_tracking_velocity + acceleration * dt, -rate, rate);
            const float next = _tracking_position + _tracking_velocity * dt;
            if ((error >= 0.0f && next >= goal) || (error <= 0.0f && next <= goal) ||
                (std::abs(error) <= 0.1f && std::abs(_tracking_velocity) <= 0.1f)) {
                _tracking_position = static_cast<float>(goal);
                _tracking_velocity = 0.0f;
                _tracking_target = false;
            } else {
                _tracking_position = uitk::clamp(next, static_cast<float>(_angle_limit.x),
                                                  static_cast<float>(_angle_limit.y));
            }
        }
        const int angle = uitk::clamp(static_cast<int>(std::lround(_tracking_position)), _angle_limit.x, _angle_limit.y);
        if (angle != static_cast<int>(_angle_anim.directValue()) || !_tracking_target) {
            _angle_anim.teleport(angle);
            set_tracking_angle_impl(angle, _tracking_rate);
        }
        return;
    }

    // Apply animation
    if (!_angle_anim.done()) {
        _angle_anim.updateWithDelta(0.02f);  // Fixed delta time for consistency
        set_angle_impl(static_cast<int>(_angle_anim.directValue()));
    }

    // Snap to target angle when animation ends
    else if (_snap_to_target_on_rest) {
        _snap_to_target_on_rest = false;
        set_angle_impl(_angle_anim.end);
    }

    // Query movement only while torque is commanded ON. Once the motion
    // finishes, release torque and leave the UART silent during standby.
    else if (_auto_torque_release_enabled && _torque_commanded_enabled &&
             GetHAL().millis() - _last_torque_check_tick > 200) {
        _last_torque_check_tick = GetHAL().millis();
        if (!isMoving()) {
            setTorqueEnabled(false);
        }
    }
}

void Servo::move(int angle)
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    apply_default_spring_options();
    update_angle_anim_target(angle);
}

void Servo::moveWithSpringParams(int angle, float stiffness, float damping)
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    _angle_anim.springOptions().visualDuration = 0.0f;  // Disable timing override
    _angle_anim.springOptions().stiffness      = stiffness;
    _angle_anim.springOptions().damping        = damping;

    update_angle_anim_target(angle);
}

void Servo::moveWithSpeed(int angle, int speed)
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    auto spring_options = map_speed_to_spring_options(speed);
    moveWithSpringParams(angle, spring_options.stiffness, spring_options.damping);
}

uint32_t Servo::trackWithSpeed(int angle, int speed)
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    const bool continuing = _tracking_target && _torque_commanded_enabled;
    const float position = continuing ? _tracking_position : static_cast<float>(
        uitk::clamp(getCurrentAngle(), _angle_limit.x, _angle_limit.y));
    const float velocity = continuing ? _tracking_velocity : 0.0f;
    cancelTimedMove();
    _tracking_rate = uitk::clamp(speed, 1, 1000);
    const auto options = map_speed_to_spring_options(_tracking_rate);
    _tracking_stiffness = options.stiffness;
    _tracking_damping = options.damping;
    _tracking_position = position;
    _tracking_velocity = velocity;
    _tracking_goal = uitk::clamp(angle, _angle_limit.x, _angle_limit.y);
    _angle_anim.teleport(static_cast<int>(std::lround(position)));
    _snap_to_target_on_rest = false;
    if (!_torque_commanded_enabled) setTorqueEnabled(true);
    if (!continuing) _tracking_tick = GetHAL().millis();
    _tracking_target = true;
    return getCommandRevision();
}

bool Servo::isTrackingCommandActive() const
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    return _tracking_target;
}

void Servo::stopTrackingIfOwned(uint32_t revision)
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    if (getCommandRevision() != revision) return;
    const int current = uitk::clamp(getCurrentAngle(), _angle_limit.x, _angle_limit.y);
    stop_motion_at_angle(current);
    if (_torque_commanded_enabled) set_angle_impl(current);
}

void Servo::moveWithDuration(int angle, uint32_t duration_ms, uint32_t now)
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    _command_revision.fetch_add(1, std::memory_order_relaxed);
    const bool was_moving = isMoving();
    _tracking_target = false;
    const int current = uitk::clamp(getCurrentAngle(), _angle_limit.x, _angle_limit.y);
    angle = uitk::clamp(angle, _angle_limit.x, _angle_limit.y);
    _angle_anim.teleport(current);
    _snap_to_target_on_rest = false;
    _timed_move.Start(current, angle, duration_ms, now);
    if (_timed_move.Active() && !_torque_commanded_enabled) setTorqueEnabled(true);
    // Replace any in-flight spring target with the sampled starting pose.
    if (was_moving && _torque_commanded_enabled) set_angle_impl(current);
}

void Servo::cancelTimedMove()
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    _command_revision.fetch_add(1, std::memory_order_relaxed);
    _timed_move.Cancel(); // Last commanded position remains; no stale spring target.
    _tracking_target = false;
}

int Servo::getCurrentAngle()
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    return _angle_anim.directValue();
}

bool Servo::isMoving()
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    // Avoid a servo status read while idle or after torque has been released.
    // Interpolation and its pending final write are tracked locally.
    if (_tracking_target || _timed_move.Active() || !_angle_anim.done() || _snap_to_target_on_rest) return true;
    return _torque_commanded_enabled && is_moving_impl();
}

void Servo::apply_default_spring_options()
{
    auto& options          = _angle_anim.springOptions();
    options.visualDuration = 0.0f;  // Disable timing override
    options.stiffness      = _default_spring_options.stiffness;
    options.damping        = _default_spring_options.damping;
}

void Servo::update_angle_anim_target(int angle)
{
    cancelTimedMove();
    angle = uitk::clamp(angle, _angle_limit.x, _angle_limit.y);

    if (_auto_angle_sync_enabled) {
        _angle_anim.teleport(getCurrentAngle());  // Use current angle as start
    }
    // Initialization and auto-release leave the actuator torque OFF.
    // A position register write alone does not re-enable it.
    if (!_torque_commanded_enabled) setTorqueEnabled(true);
    _angle_anim             = angle;  // Apply new target
    _snap_to_target_on_rest = true;
}

void Servo::stop_motion_at_angle(int angle)
{
    std::lock_guard<std::recursive_mutex> lock(_state_mutex);
    cancelTimedMove(); // A stall must also stop timed motion.
    angle = uitk::clamp(angle, _angle_limit.x, _angle_limit.y);
    _angle_anim.teleport(angle);
    _snap_to_target_on_rest = false;
}

uitk::SpringOptions_t Servo::map_speed_to_spring_options(int speed)
{
    speed = uitk::clamp(speed, 0, 1000);

    // 1. 计算 Stiffness (刚度)
    // 使用二次方映射: k = k_min + (speed/1000)^2 * k_range
    // 当 speed=500 时，k 约为 10 + 0.25 * 640 = 170
    float k_min           = 10.0f;
    float k_max           = 650.0f;
    float normalizedSpeed = speed / 1000.0f;
    float stiffness       = k_min + (normalizedSpeed * normalizedSpeed) * (k_max - k_min);

    // 2. 计算 Damping (阻尼)
    // 为了保持临界阻尼(无过冲，最快稳定)，公式为 d = 2 * sqrt(m * k)
    // 如果想要带一点点弹性感(bounce)，可以将系数从 2.0 降到 1.5~1.8
    float mass    = 1.0f;
    float damping = 2.0f * sqrtf(mass * stiffness);

    // 3. 构造选项
    uitk::SpringOptions_t options = _default_spring_options;
    options.stiffness             = stiffness;
    options.damping               = damping;
    options.mass                  = mass;

    // 4. 动态调整静止阈值
    // 高速时阈值大一点可以防止由于离散计算导致的微小抖动
    if (speed > 800) {
        options.restDelta = 0.5f;
        options.restSpeed = 0.5f;
    } else {
        options.restDelta = 0.1f;
        options.restSpeed = 0.1f;
    }

    return options;
}

}  // namespace stackchan::motion
