/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/FTServo_Arduino/src/SCSCL.h"
#include <stackchan/stackchan.h>
#include <smooth_ui_toolkit.hpp>
#include <mooncake_log.h>
#include <settings.h>
#include <mutex>
#include <cstdlib>

using namespace smooth_ui_toolkit;
using namespace stackchan::motion;

static SCSCL _scs_bus;
static std::mutex _scs_bus_mutex;

struct ServoConfig_t {
    int id             = -1;
    int defaultZeroPos = 0;
    Vector2i angleLimit;
    Vector2i rawPosLimit;
    std::string settingNs;
    std::string settingZeroPositionKey;
    bool enablePwmMode = false;
    bool enableStallProtection = false;
};

class ScsServo : public Servo {
public:
    static inline const std::string _tag = "ScsServo";

    ScsServo(const ServoConfig_t& config) : _config(config), _runtime_raw_pos_limit(config.rawPosLimit)
    {
    }

    void init() override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        reset_runtime_limits();
        set_angle_limit(_config.angleLimit);
        get_zero_pos_from_nvs();
        Servo::init();
    }

    void get_zero_pos_from_nvs()
    {
        _zero_pos     = _config.defaultZeroPos;
        bool is_valid = false;

        {
            Settings settings(_config.settingNs, false);
            int nvs_zero_pos = settings.GetInt(_config.settingZeroPositionKey, -1);

            // Limit check
            if (nvs_zero_pos >= _config.rawPosLimit.x && nvs_zero_pos <= _config.rawPosLimit.y) {
                _zero_pos = nvs_zero_pos;
                is_valid  = true;
                mclog::tagInfo(_tag, "id: {} get zero pos: {} from settings", _config.id, _zero_pos);
            } else {
                is_valid = false;
                mclog::tagWarn(_tag, "id: {} get invalid zero pos: {} from settings", _config.id, nvs_zero_pos);
            }
        }

        if (!is_valid) {
            _zero_pos = _config.defaultZeroPos;
            mclog::tagInfo(_tag, "id: {} override zero pos to default: {}", _config.id, _zero_pos);

            Settings settings(_config.settingNs, true);
            settings.SetInt(_config.settingZeroPositionKey, _zero_pos);
        }
    }

    void set_angle_impl(int angle) override
    {
        writeAngle(angle, 0);
    }

    void set_tracking_angle_impl(int angle, int rate) override
    {
        writeAngle(angle, rate);
    }

    void writeAngle(int angle, int rate)
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        int mapped_angle = _zero_pos + angle * 16 / 5 / 10;  // 一步对应 0.3125度, 0.3125 = 5/16
        mapped_angle     = uitk::clamp(mapped_angle, _runtime_raw_pos_limit.x, _runtime_raw_pos_limit.y);

        // mclog::tagInfo(_tag, "id: {} mapped angle: {}", _id, mapped_angle);

        if (update_stall_protection(mapped_angle)) {
            return;
        }

        check_mode(Mode::Position);
        // Keep the existing tracking rate calculation and duplicate suppression.
        // Ordinary animation can repeat the same quantized position. Only skip
        // an acknowledged identical command within the same motion revision.
        const uint32_t revision = getCommandRevision();
        const uint32_t now = GetHAL().millis();
        if (rate > 0 && _last_tracking_raw == mapped_angle) {
            ++_tracking_write_skipped;
            return;
        }
        if (rate == 0 && _last_normal_raw == mapped_angle &&
            _last_normal_revision == revision &&
            now - _last_normal_write_tick < kNormalRefreshMs) {
            ++_normal_write_skipped;
            report_bus_stats();
            return;
        }
        uint16_t duration = 20;
        if (rate > 0 && _last_tracking_raw >= 0) {
            // SCS0009 position quantum is 0.3125 degrees. Time is ms.
            // Rounded up so the requested segment does not exceed the rate.
            const int delta = std::abs(mapped_angle - _last_tracking_raw);
            const int ms = (delta * 3125 + rate - 1) / rate;
            duration = static_cast<uint16_t>(uitk::clamp(ms, 20, 65535));
        }
        const auto ack_before = _scs_bus.getWriteAckDiagnostic(_config.id).received;
        _scs_bus.WritePos(_config.id, mapped_angle, duration, 0);
        _last_tracking_raw = rate > 0 ? mapped_angle : -1;
        _last_normal_raw = -1;
        if (rate == 0) {
            ++_normal_write_sent;
            if (_scs_bus.getWriteAckDiagnostic(_config.id).received != ack_before) {
                _last_normal_raw = mapped_angle;
                _last_normal_revision = revision;
                _last_normal_write_tick = GetHAL().millis();
            }
        } else {
            ++_tracking_write_sent;
        }
    }

    int getCurrentAngle() override
    {
        int angle = 0;
        if (tryGetCurrentAngle(angle)) return angle;
        return uitk::clamp(Servo::getCurrentAngle(), getAngleLimit().x, getAngleLimit().y);
    }

    bool tryGetCurrentAngle(int& angle) override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        uint8_t communication_error = 0;
        int current_pos = -1;
        if (is_raw_pos_valid(_feedback_position) && GetHAL().millis() - _feedback_tick <= 10) {
            current_pos = _feedback_position;
            ++_read_shared;
        } else {
            current_pos = read_position_with_retry(communication_error);
        }
        _feedback_position = -1; // a physical sample is shared at most once
        report_bus_stats();
        if (!is_raw_pos_valid(current_pos)) {
            int fallback_angle = uitk::clamp(Servo::getCurrentAngle(), getAngleLimit().x, getAngleLimit().y);
            mclog::tagWarn(_tag,
                           "id: {} ignore invalid current pos after retry: {}, error: {} ({}), fallback angle: {}",
                           _config.id, current_pos, communication_error_name(communication_error),
                           static_cast<unsigned>(communication_error), fallback_angle);
            return false;
        }

        angle = raw_pos_to_angle(current_pos);
        angle     = uitk::clamp(angle, getAngleLimit().x, getAngleLimit().y);
        // mclog::tagInfo(_tag, "id: {} current pos: {} angle: {}", _id, current_pos, angle);
        return true;
    }

    bool is_moving_impl() override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        int moving = _scs_bus.ReadMove(_config.id);
        // A failed UART read returns -1; it is not evidence of movement.
        // The animation state is already checked by Servo::isMoving().
        return moving > 0;
    }

    void setTorqueEnabled(bool enabled) override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        // Write ACK is optional and collected by the bus with a short timeout.
        if (_torque_command_known && _torque_commanded_enabled == enabled) return;
        _torque_command_known = true;
        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_normal_raw = -1;
        _scs_bus.EnableTorque(_config.id, enabled ? 1 : 0);
        Servo::setTorqueEnabled(enabled);
    }

    bool getTorqueEnabled() override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        int torque_enable = _scs_bus.ReadToqueEnable(_config.id);
        // mclog::tagInfo(_tag, "id: {} torque enable: {}", _id, torque_enable);
        return torque_enable > 0;
    }

    void setCurrentAngleAsZero() override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        uint8_t communication_error = 0;
        int current_pos = read_position_with_retry(communication_error);
        if (!is_raw_pos_valid(current_pos)) {
            mclog::tagWarn(_tag,
                           "id: {} ignore invalid zero calibration pos after retry: {}, error: {} ({}), keep zero pos: {}",
                           _config.id, current_pos, communication_error_name(communication_error),
                           static_cast<unsigned>(communication_error), _zero_pos);
            return;
        }

        _zero_pos = current_pos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);

        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, _zero_pos);
    }

    void resetZeroCalibration() override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        _zero_pos = _config.defaultZeroPos;
        reset_runtime_limits();

        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _zero_pos);

        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, _zero_pos);
    }

    void rotate(int velocity) override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        velocity = uitk::clamp(velocity, -1000, 1000);

        if (!_config.enablePwmMode) {
            return;
        }

        int mapped_velocity = map_range(velocity, 0, 1000, 0, 1023);

        check_mode(Mode::PWM);
        _scs_bus.WritePWM(_config.id, mapped_velocity);
    }

private:
    enum class Mode { Position = 0, PWM = 1 };

    ServoConfig_t _config;
    Vector2i _runtime_raw_pos_limit;
    int _zero_pos      = 0;
    Mode _current_mode = Mode::Position;
    bool _torque_command_known = false;
    int _last_tracking_raw = -1;
    int _last_normal_raw = -1;
    uint32_t _last_normal_revision = 0, _last_normal_write_tick = 0;
    uint32_t _normal_write_sent = 0, _normal_write_skipped = 0;
    uint32_t _tracking_write_sent = 0, _tracking_write_skipped = 0;
    static constexpr uint32_t kNormalRefreshMs = 250;
    int _feedback_position = -1;
    uint32_t _feedback_tick = 0;
    uint32_t _read_ok = 0, _read_retry_ok = 0, _read_fail = 0, _read_shared = 0;
    uint32_t _stall_ok = 0, _stall_fail = 0, _diag_tick = 0;
    uint32_t _stall_retry_ok = 0, _stall_retry_fail = 0;
    uint32_t _stall_last_valid_tick = 0;
    uint32_t _stall_errors[5] = {}; // 0: invalid range/other, 1..4: SCS_ERR_LIST

    static constexpr uint32_t kStallFeedbackIntervalMs = 50;
    static constexpr int kStallMinTargetDeltaRaw       = 8;
    static constexpr int kStallMaxPositionDeltaRaw     = 1;
    static constexpr int kStallCurrentRiseThreshold    = 80;
    static constexpr int kStallLoadRiseThreshold       = 150;
    static constexpr int kStallCurrentAbsThreshold     = 350;
    static constexpr int kStallLoadAbsThreshold        = 650;
    static constexpr int kStallConfirmSamples          = 2;

    uint32_t _last_stall_check_tick = 0;
    int _last_stall_raw_pos         = 0;
    int _last_stall_current_abs     = 0;
    int _last_stall_load_abs        = 0;
    int _last_stall_direction       = 0;
    int _stall_confirm_count        = 0;
    bool _last_stall_feedback_valid = false;

    static const char* communication_error_name(uint8_t error)
    {
        switch (error) {
            case 0: return "invalid_range";
            case ERR_NO_REPLY: return "no_reply";
            case ERR_CRC_CMP: return "checksum";
            case ERR_SLAVE_ID: return "wrong_id";
            case ERR_BUFF_LEN: return "wrong_length";
            default: return "unknown";
        }
    }

    // Caller holds _scs_bus_mutex. A single retry filters an isolated UART
    // reply loss without hiding a persistent wiring, power or servo fault.
    int read_position_with_retry(uint8_t& communication_error)
    {
        int current_pos = _scs_bus.ReadPos(_config.id);
        communication_error = _scs_bus.getLastError();
        if (is_raw_pos_valid(current_pos)) { ++_read_ok; return current_pos; }
        if (current_pos < 0 && communication_error != 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            current_pos = _scs_bus.ReadPos(_config.id);
            communication_error = _scs_bus.getLastError();
            if (is_raw_pos_valid(current_pos)) { ++_read_retry_ok; return current_pos; }
        }
        ++_read_fail;
        return current_pos;
    }

    void report_bus_stats()
    {
        const uint32_t now = GetHAL().millis();
        if (now - _diag_tick < 10000) return;
        _diag_tick = now;
        mclog::tagInfo(_tag, "[SERVO_BUS] id={} pos_ok={} retry_ok={} pos_fail={} shared={} stall_ok={} stall_fail={}",
                       _config.id, _read_ok, _read_retry_ok, _read_fail, _read_shared, _stall_ok, _stall_fail);
        mclog::tagInfo(_tag, "[SERVO_WRITE_RATE] id={} normal_sent={} normal_skipped={} tracking_sent={} tracking_skipped={}",
                       _config.id, _normal_write_sent, _normal_write_skipped,
                       _tracking_write_sent, _tracking_write_skipped);
        const auto& ack = _scs_bus.getWriteAckDiagnostic(_config.id);
        mclog::tagInfo(_tag, "[SERVO_WRITE_ACK] id={} received={} absent={} invalid={}",
                       _config.id, ack.received, ack.absent, ack.invalid);
        for (int kind = 0; kind < 2; ++kind) {
            const auto& d = _scs_bus.getReadHeaderDiagnostic(_config.id, kind != 0);
            if (!d.wrong_id && !d.wrong_length) continue;
            mclog::tagInfo(_tag, "[SERVO_RX_HEADER] id={} long_read={} wrong_id={} wrong_length={} rx_ff={} rx_other_servo={} rx_zero={} rx_other={} ack_length={} read_echo_prefix={} ack_skipped={}",
                           _config.id, kind, d.wrong_id, d.wrong_length, d.rx_ff, d.rx_other_servo,
                           d.rx_zero, d.rx_other, d.ack_length, d.read_echo_prefix, d.ack_skipped);
            mclog::tagInfo(_tag, "[SERVO_RX_LAST] id={} long_read={} addr={} bytes={} rx_id={} rx_len={} rx_status={} prev_id={} prev_cmd={} prev_addr={}",
                           _config.id, kind, static_cast<unsigned>(d.address), static_cast<unsigned>(d.requested_bytes),
                           static_cast<unsigned>(d.rx_id), static_cast<unsigned>(d.rx_length), static_cast<unsigned>(d.rx_status),
                           static_cast<unsigned>(d.previous_id), static_cast<unsigned>(d.previous_command), static_cast<unsigned>(d.previous_address));
        }
        if (_config.enableStallProtection) {
            mclog::tagInfo(_tag, "[SERVO_STALL_BUS] id={} no_reply={} checksum={} wrong_id={} wrong_length={} invalid_other={} retry_ok={} retry_fail={}",
                           _config.id, _stall_errors[ERR_NO_REPLY], _stall_errors[ERR_CRC_CMP],
                           _stall_errors[ERR_SLAVE_ID], _stall_errors[ERR_BUFF_LEN], _stall_errors[0],
                           _stall_retry_ok, _stall_retry_fail);
        }
    }

    static int abs_int(int value)
    {
        return value < 0 ? -value : value;
    }

    bool is_raw_pos_valid(int raw_pos) const
    {
        return raw_pos >= _config.rawPosLimit.x && raw_pos <= _config.rawPosLimit.y;
    }

    int raw_pos_to_angle(int raw_pos) const
    {
        return (raw_pos - _zero_pos) * 5 * 10 / 16;
    }

    void reset_runtime_limits()
    {
        _runtime_raw_pos_limit = _config.rawPosLimit;
        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_normal_raw = -1;
        set_angle_limit(_config.angleLimit);
        reset_stall_detection();
    }

    void reset_stall_detection()
    {
        _last_stall_feedback_valid = false;
        _last_stall_direction      = 0;
        _stall_confirm_count       = 0;
    }

    bool update_stall_protection(int target_raw_pos)
    {
        if (!_config.enableStallProtection) {
            return false;
        }

        const uint32_t now = GetHAL().millis();
        if (now - _last_stall_check_tick < kStallFeedbackIntervalMs) {
            return false;
        }
        _last_stall_check_tick = now;

        _feedback_position = -1;
        int feedback = _scs_bus.FeedBack(_config.id);
        // A single retry is useful only after an isolated lost sample.
        // Do not double bus load throughout a persistent outage.
        if (feedback < 0 && _last_stall_feedback_valid) {
            feedback = _scs_bus.FeedBack(_config.id);
            if (feedback < 0) ++_stall_retry_fail;
            else ++_stall_retry_ok;
        }
        if (feedback < 0) {
            ++_stall_fail;
            const unsigned error = _scs_bus.getLastError();
            ++_stall_errors[error <= ERR_BUFF_LEN ? error : 0];
            report_bus_stats();
            reset_stall_detection();
            return false;
        }

        const int current_raw_pos = _scs_bus.ReadPos(-1);
        const int current_abs     = abs_int(_scs_bus.ReadCurrent(-1));
        const int load_abs        = abs_int(_scs_bus.ReadLoad(-1));

        if (!is_raw_pos_valid(current_raw_pos)) {
            ++_stall_fail;
            ++_stall_errors[0];
            report_bus_stats();
            reset_stall_detection();
            return false;
        }
        ++_stall_ok;
        _feedback_position = current_raw_pos;
        _feedback_tick = GetHAL().millis();
        // Do not compare stale samples after a long interruption.
        if (_feedback_tick - _stall_last_valid_tick > 150) reset_stall_detection();
        _stall_last_valid_tick = _feedback_tick;
        report_bus_stats();

        const int target_delta = target_raw_pos - current_raw_pos;
        if (abs_int(target_delta) < kStallMinTargetDeltaRaw) {
            reset_stall_detection();
            return false;
        }

        const int direction = target_delta > 0 ? 1 : -1;
        if (_last_stall_feedback_valid && direction == _last_stall_direction) {
            const int pos_delta = abs_int(current_raw_pos - _last_stall_raw_pos);
            const bool position_stuck = pos_delta <= kStallMaxPositionDeltaRaw;
            const bool current_spike  = current_abs >= kStallCurrentAbsThreshold ||
                                       current_abs - _last_stall_current_abs >= kStallCurrentRiseThreshold;
            const bool load_spike = load_abs >= kStallLoadAbsThreshold ||
                                    load_abs - _last_stall_load_abs >= kStallLoadRiseThreshold;

            if (position_stuck && (current_spike || load_spike)) {
                _stall_confirm_count++;
            } else if (pos_delta > kStallMaxPositionDeltaRaw) {
                _stall_confirm_count = 0; 
            }
        } else {
            _stall_confirm_count = 0;
        }

        _last_stall_raw_pos         = current_raw_pos;
        _last_stall_current_abs     = current_abs;
        _last_stall_load_abs        = load_abs;
        _last_stall_direction       = direction;
        _last_stall_feedback_valid = true;

        if (_stall_confirm_count < kStallConfirmSamples) {
            return false;
        }

        handle_stall(current_raw_pos, direction, current_abs, load_abs);
        return true;
    }

    void handle_stall(int raw_pos, int direction, int current_abs, int load_abs)
    {
        int angle = raw_pos_to_angle(raw_pos);
        angle     = uitk::clamp(angle, _config.angleLimit.x, _config.angleLimit.y);

        auto angle_limit = getAngleLimit();
        if (direction > 0) {
            if (raw_pos < _runtime_raw_pos_limit.y) {
                _runtime_raw_pos_limit.y = raw_pos;
            }
            if (angle < angle_limit.y) {
                angle_limit.y = angle;
            }
        } else {
            if (raw_pos > _runtime_raw_pos_limit.x) {
                _runtime_raw_pos_limit.x = raw_pos;
            }
            if (angle > angle_limit.x) {
                angle_limit.x = angle;
            }
        }
        set_angle_limit(angle_limit);
        stop_motion_at_angle(angle);
        reset_stall_detection();

        check_mode(Mode::Position);
        _scs_bus.WritePos(_config.id, raw_pos, 20, 0);
        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_normal_raw = -1;

        mclog::tagWarn(_tag,
                       "id: {} stall detected, raw: {}, angle: {}, dir: {}, current: {}, load: {}, limit: [{}, {}]",
                       _config.id, raw_pos, angle, direction, current_abs, load_abs, angle_limit.x, angle_limit.y);
    }

    void check_mode(Mode targetMode)
    {
        if (targetMode == _current_mode) {
            return;
        }

        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_normal_raw = -1;
        _scs_bus.SwitchMode(_config.id, static_cast<uint8_t>(targetMode));
        _current_mode = targetMode;
    }
};

void Hal::servo_init()
{
    mclog::tagInfo("HAL-Servo", "init");

    if (!_scs_bus.begin(UART_NUM_1, 1000000, 6, 7)) {
        mclog::tagError("HAL-Servo", "failed to initialize UART1");
    }
    // Keep optional write ACK semantics; the driver collects and validates
    // replies without making a missing ACK a mandatory write failure.
    _scs_bus.Level = 0;

    ServoConfig_t yaw_servo_config;
    yaw_servo_config.id                     = 1;
    yaw_servo_config.defaultZeroPos         = 460;
    yaw_servo_config.angleLimit             = Vector2i(-1280, 1280);
    yaw_servo_config.rawPosLimit            = Vector2i(0, 1000);
    yaw_servo_config.settingNs              = "servo";
    yaw_servo_config.settingZeroPositionKey = "zero_pos_1";
    yaw_servo_config.enablePwmMode          = true;

    ServoConfig_t pitch_servo_config;
    pitch_servo_config.id                     = 2;
    pitch_servo_config.defaultZeroPos         = 620;
    pitch_servo_config.angleLimit             = Vector2i(30, 870);
    pitch_servo_config.rawPosLimit            = Vector2i(0, 1000);
    pitch_servo_config.settingNs              = "servo";
    pitch_servo_config.settingZeroPositionKey = "zero_pos_2";
    pitch_servo_config.enableStallProtection  = true;

    auto yaw_servo   = std::make_unique<ScsServo>(yaw_servo_config);
    auto pitch_servo = std::make_unique<ScsServo>(pitch_servo_config);
    auto motion      = std::make_unique<Motion>(std::move(yaw_servo), std::move(pitch_servo));
    motion->init();

    GetStackChan().attachMotion(std::move(motion));
}
