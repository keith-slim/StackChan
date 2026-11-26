/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <smooth_ui_toolkit.hpp>
#include <uitk/short_namespace.hpp>
#include <cstdint>
#include <atomic>
#include <mutex>
#include "timed_servo_move.h"

namespace stackchan::motion {

/**
 * @brief
 *
 */
class Servo {
public:
    virtual ~Servo() = default;

    /**
     * @brief
     *
     */
    virtual void init();

    /**
     * @brief
     *
     */
    virtual void update();

    /**
     * @brief Move to angle
     *
     * @param angle
     */
    void move(int angle);

    /**
     * @brief Move to angle with custom spring params
     *
     * @param angle
     * @param stiffness
     * @param damping
     */
    void moveWithSpringParams(int angle, float stiffness = 170.0f, float damping = 26.0f);

    /**
     * @brief Move to angle with speed mapping
     *
     * @param angle
     * @param speed (0-1000)
     */
    void moveWithSpeed(int angle, int speed);
    // Rate-limited spring; speed is in tenths of a degree per second.
    uint32_t trackWithSpeed(int angle, int speed, const int* measured_start = nullptr);
    bool isTrackingCommandActive() const;
    bool startHardwareSweep(int angle, int rate, uint32_t& revision);
    bool stopHardwareSweep(uint32_t revision);
    // Hardware feedback must be distinguished from the interpolated fallback.
    virtual bool tryGetCurrentAngle(int& angle) { angle = getCurrentAngle(); return true; }
    void stopTrackingIfOwned(uint32_t revision, const int* measured_stop = nullptr);

    // One physical position sync at entry, then direct timed setpoints.
    void moveWithDuration(int angle, uint32_t duration_ms, uint32_t now);
    void cancelTimedMove();
    // An expression may restore only a target it still owns. This revision
    // changes on external commands, cancellation and stall protection.
    uint32_t getCommandRevision() const { return _command_revision.load(std::memory_order_relaxed); }

    /**
     * @brief Rotate servo with given velocity
     *
     * @param velocity (-1000, 1000)
     */
    virtual void rotate(int velocity)
    {
    }

    /**
     * @brief Get servo current angle
     *
     * @return int
     */
    virtual int getCurrentAngle();

    /**
     * @brief
     *
     * @return uitk::Vector2i
     */
    virtual uitk::Vector2i getAngleLimit() const
    {
        std::lock_guard<std::recursive_mutex> lock(_state_mutex);
        return _angle_limit;
    }

    /**
     * @brief
     *
     * @return true
     * @return false
     */
    bool isMoving();

    /**
     * @brief
     *
     * @param enabled
     */
    virtual void setTorqueEnabled(bool enabled)
    {
        std::lock_guard<std::recursive_mutex> lock(_state_mutex);
        _torque_commanded_enabled = enabled;
    }
    virtual bool getTorqueEnabled()
    {
        return false;
    }

    /**
     * @brief Auto release torque on rest
     *
     * @param enabled
     */
    void setAutoTorqueReleaseEnabled(bool enabled)
    {
        std::lock_guard<std::recursive_mutex> lock(_state_mutex);
        _auto_torque_release_enabled = enabled;
    }

    /**
     * @brief Enables or disables automatic synchronization of the animation start point
     *        with the current physical angle.
     *
     * @param enabled
     *        - If true: Prevents sudden "jumps" when the servo is moved manually by
     *          external forces, but may cause stuttering during high-frequency updates
     *          as it resets the animation's velocity.
     *        - If false: Maintains animation momentum and velocity for smooth,
     *          continuous motion, but may cause a "snap" if the actual angle differs
     *          significantly from the internal state.
     */
    void setAutoAngleSyncEnabled(bool enabled)
    {
        std::lock_guard<std::recursive_mutex> lock(_state_mutex);
        _auto_angle_sync_enabled = enabled;
    }

    /**
     * @brief
     *
     */
    virtual void setCurrentAngleAsZero()
    {
    }

    /**
     * @brief
     *
     */
    virtual void resetZeroCalibration()
    {
    }

protected:
    Servo()
    {
    }

    void set_angle_limit(uitk::Vector2i angleLimit)
    {
        std::lock_guard<std::recursive_mutex> lock(_state_mutex);
        _angle_limit = angleLimit;
    }

    /**
     * @brief Servo set angle implementation
     *
     * @param angle
     */
    virtual void set_angle_impl(int angle) = 0;
    virtual void set_tracking_angle_impl(int angle, int rate) { set_angle_impl(angle); }
    virtual bool write_hardware_sweep(int angle, int rate, int measured) { return false; }
    virtual bool hold_hardware_sweep(int& measured) { return false; }
    virtual bool is_moving_impl()
    {
        return false;
    }

protected:
    // Always acquire this before the shared hardware bus mutex.
    mutable std::recursive_mutex _state_mutex;
    uitk::Vector2i _angle_limit;
    uitk::AnimateValue _angle_anim;
    TimedServoMove _timed_move;
    std::atomic<uint32_t> _command_revision{0};

    uint32_t _last_tick               = 0;
    uint32_t _last_torque_check_tick  = 0;
    enum class Profile { Idle, Spring, Timed, Tracking, HardwareSweep };
    Profile _profile = Profile::Idle;
    int _goal = 0;
    float _tracking_position = 0.0f;
    float _tracking_velocity = 0.0f;
    int _tracking_rate = 0;
    float _tracking_stiffness = 0.0f;
    float _tracking_damping = 0.0f;
    uint32_t _tracking_tick = 0;
    bool _auto_torque_release_enabled = true;
    bool _auto_angle_sync_enabled     = true;
    // Track commands instead of polling the servo while idle with torque off.
    bool _torque_commanded_enabled    = false;

    void cancel_profile(bool preserve_spring = false);
    void apply_default_spring_options();
    void update_angle_anim_target(int angle);
    void stop_motion_at_angle(int angle);
    uitk::SpringOptions_t map_speed_to_spring_options(int speed);
};

}  // namespace stackchan::motion
