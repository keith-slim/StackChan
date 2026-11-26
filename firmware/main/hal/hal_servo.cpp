/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/FTServo_Arduino/src/SCSCL.h"
#include <stackchan/stackchan.h>
#include <stackchan/motion/servo_executor.h>
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

    void update() override
    {
        if (executor::external()) {
            executor::invoke([&] { update(); });
            return;
        }
        Servo::update();
        // Continue monitoring the last physical setpoint even when the
        // interpolation has finished or its quantized position is unchanged.
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        {
            std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
            if (_config.enableStallProtection && _torque_commanded_enabled &&
                _current_mode == Mode::Position && _monitor_target >= 0) {
                update_stall_protection(_monitor_target);
            }
        }
        report_bus_stats();
    }

    int _sweep_hold_raw = -1;
    int _sweep_target_raw = -1;
    int _sweep_rate = 0;
    uint32_t _sweep_sent_tick = 0, _sweep_check_tick = 0;
    bool _sweep_accepted = false;
    bool _sweep_hold_accepted = false;
    unsigned _sweep_retries = 0;
    bool supports_hardware_sweep() const override { return true; }
    int angle_to_raw(int angle) const {
        return uitk::clamp(_zero_pos + angle * 16 / 5 / 10,
                           _runtime_raw_pos_limit.x, _runtime_raw_pos_limit.y);
    }
    bool send_position(int raw, uint16_t duration)
    {
        const auto before = _scs_bus.getWriteAckDiagnostic(_config.id).received;
        _scs_bus.WritePos(_config.id, raw, duration, 0);
        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_ack_raw = -1;
        _monitor_target = raw;
        return _scs_bus.getWriteAckDiagnostic(_config.id).received != before && _scs_bus.getState() == 0;
    }
    bool issue_sweep(int raw, int rate, int start)
    {
        const int duration = uitk::clamp((std::abs(raw - start) * 3125 + rate - 1) / rate, 20, 9999);
        _sweep_sent_tick = GetHAL().millis();
        return send_position(raw, duration);
    }
    bool write_hardware_sweep(int angle, int rate, int measured) override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::unique_lock<std::mutex> bus_lock(_scs_bus_mutex);
        _sweep_hold_raw = -1;
        _sweep_hold_accepted = false;
        _sweep_target_raw = angle_to_raw(angle);
        _sweep_rate = uitk::clamp(rate, 1, 1000);
        _sweep_retries = 0;
        check_mode(Mode::Position);
        const int start = angle_to_raw(measured);
        if (sweep_cancelled()) { requestHardwareStop("detection_or_end"); return false; }
        _sweep_accepted = issue_sweep(_sweep_target_raw, _sweep_rate, start);
        _sweep_check_tick = _sweep_sent_tick;
        const bool ack = _sweep_accepted;
        bus_lock.unlock();
        ESP_LOGI("ScsServo", "[SWEEP_START] id=%d rev=%u measured=%d target=%d raw=%d rate=%d ack=%d",
                 _config.id, getCommandRevision(), measured, angle, _sweep_target_raw, rate, ack);
        return ack;
    }

    void service_hardware_sweep() override
    {
        if (_sweep_accepted || GetHAL().millis() - _sweep_check_tick < 100) return;
        _sweep_check_tick = GetHAL().millis();
        std::unique_lock<std::mutex> bus_lock(_scs_bus_mutex);
        const int goal = _scs_bus.readWord(_config.id, SCSCL_GOAL_POSITION_L);
        if (goal == _sweep_target_raw) _sweep_accepted = true;
        // A missing read is not a rejected command. Retry only a confirmed
        // mismatch, using a new measured starting point and a bounded count.
        else if (goal >= 0 && _sweep_retries < 2) {
            uint8_t error = 0;
            const int raw = read_position_with_retry(error);
            if (is_raw_pos_valid(raw) && !_hardware_stop_requested.load() && !sweep_cancelled()) {
                ++_sweep_retries;
                _sweep_accepted = issue_sweep(_sweep_target_raw, _sweep_rate, raw);
            }
        }
        const bool accepted = _sweep_accepted;
        const unsigned retries = _sweep_retries;
        bus_lock.unlock();
        ESP_LOGI("ScsServo", "[SWEEP_ACCEPT] id=%d goal=%d accepted=%d retries=%u", _config.id, goal, accepted, retries);
        if (!_sweep_accepted && ((retries >= 2 && goal >= 0) || GetHAL().millis() - _sweep_sent_tick > 8000)) requestHardwareStop("command_unconfirmed");
    }

    bool hold_hardware_sweep(int& measured) override
    {
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::unique_lock<std::mutex> bus_lock(_scs_bus_mutex);
        const uint32_t started = GetHAL().millis();
        bool ack = false;
        uint8_t error = 0;
        if (_sweep_hold_raw < 0) {
            // A sample from this instant is preferred. Do not stop at a stale
            // interpolated position or repeatedly move the hold target.
            int raw = read_position_with_retry(error);
            if (!is_raw_pos_valid(raw)) return false;
            _sweep_hold_raw = raw;
            ack = send_position(raw, 20);
            _sweep_hold_accepted = ack;
            _sweep_check_tick = GetHAL().millis();
            bus_lock.unlock();
            ESP_LOGI("ScsServo", "[SWEEP_HOLD_SENT] id=%d rev=%u raw=%d ack=%d reason=%s elapsed_ms=%u",
                     _config.id, getCommandRevision(), raw, ack, _hardware_stop_reason.load(),
                     static_cast<unsigned>(GetHAL().millis()-started));
            return false;
        }
        if (GetHAL().millis() - _sweep_check_tick < 40) return false;
        _sweep_check_tick = GetHAL().millis();
        const int actual = read_position_with_retry(error);
        if (!is_raw_pos_valid(actual)) return false;
        const int moving = _scs_bus.ReadMove(_config.id);
        // Actual stopped feedback is sufficient if it matches the hold.
        // Never interpret a missing register read as a mismatched target.
        int goal = -1;
        if (!_sweep_hold_accepted || (moving == 0 && std::abs(actual - _sweep_hold_raw) > 2)) {
            goal = _scs_bus.readWord(_config.id, SCSCL_GOAL_POSITION_L);
            if (goal == _sweep_hold_raw) _sweep_hold_accepted = true;
        }
        const bool verified = _sweep_hold_accepted && moving == 0 && std::abs(actual - _sweep_hold_raw) <= 2;
        if (!verified && moving >= 0) {
            if (goal >= 0 && goal != _sweep_hold_raw) {
                // Rejected hold: sample a new current position, not an old
                // position that would pull the moving actuator backwards.
                _sweep_hold_raw = -1;
                _sweep_hold_accepted = false;
            } else if (_sweep_hold_accepted && moving == 0) {
                // Actuator reports stopped at a small offset. Preserve the
                // measured pose rather than repeatedly hunting the hold.
                measured = raw_pos_to_angle(actual);
                _sweep_hold_raw = -1;
                bus_lock.unlock();
                ESP_LOGI("ScsServo", "[SWEEP_STOP] id=%d measured=%d verified=1 settled_offset=1", _config.id, measured);
                return true;
            }
        }
        measured = raw_pos_to_angle(actual);
        if (verified) _sweep_hold_raw = -1;
        bus_lock.unlock();
        ESP_LOGI("ScsServo", "[SWEEP_STOP] id=%d actual_raw=%d goal_raw=%d moving=%d verified=%d elapsed_ms=%u",
                 _config.id, actual, goal, moving, verified, static_cast<unsigned>(GetHAL().millis()-started));
        return verified;
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
        if (executor::external()) {
            executor::invoke([&] { writeAngle(angle, rate); });
            return;
        }
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        const int mapped_angle = angle_to_raw(angle);

        // mclog::tagInfo(_tag, "id: {} mapped angle: {}", _id, mapped_angle);

        _monitor_target = mapped_angle;
        if (update_stall_protection(mapped_angle)) {
            return;
        }

        check_mode(Mode::Position);
        // Keep the existing tracking rate calculation and duplicate suppression.
        // Ordinary animation can repeat the same quantized position. Only skip
        // an acknowledged identical command within the same motion revision.
        const uint32_t revision = getCommandRevision();
        const uint32_t now = GetHAL().millis();
        const bool duplicate = _last_ack_raw == mapped_angle && _last_ack_rate == rate &&
                               (rate > 0 || _last_ack_revision == revision) &&
                               now - _last_ack_tick < kNormalRefreshMs;
        if (duplicate) {
            if (rate > 0) ++_tracking_write_skipped;
            else ++_normal_write_skipped;
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
        if (!_feedback_bulk) _feedback_position = -1;
        const bool acknowledged = _scs_bus.getWriteAckDiagnostic(_config.id).received != ack_before &&
                                  _scs_bus.getState() == 0;
        _last_tracking_raw = rate > 0 ? mapped_angle : -1;
        _last_ack_raw = acknowledged ? mapped_angle : -1;
        _last_ack_rate = rate;
        _last_ack_revision = revision;
        _last_ack_tick = GetHAL().millis();
        if (rate == 0) ++_normal_write_sent;
        else ++_tracking_write_sent;
    }

    int getCurrentAngle() override
    {
        if (executor::external()) {
            int result{};
            executor::invoke([&] { result = getCurrentAngle(); });
            return result;
        }
        int angle = 0;
        if (tryGetCurrentAngle(angle)) return angle;
        std::lock_guard<std::recursive_mutex> lock(_state_mutex);
        const int fallback = _profile == Profile::HardwareSweep && is_raw_pos_valid(_last_valid_position)
            ? raw_pos_to_angle(_last_valid_position) : Servo::getCurrentAngle();
        return uitk::clamp(fallback, getAngleLimit().x, getAngleLimit().y);
    }

    bool tryGetCurrentAngle(int& angle) override
    {
        uint32_t sampled_at = 0;
        return tryGetMeasuredAngle(angle, sampled_at);
    }

    bool tryGetMeasuredAngle(int& angle, uint32_t& sampled_at) override
    {
        if (executor::external()) {
            bool result{};
            executor::invoke([&] { result = tryGetMeasuredAngle(angle, sampled_at); });
            return result;
        }
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::unique_lock<std::mutex> bus_lock(_scs_bus_mutex);
        uint8_t communication_error = 0;
        int current_pos = -1;
        // Both short reads and bulk feedback populate the same measured cache.
        // Limit reuse to 10 ms for short reads, and 50 ms for bulk feedback.
        // Do not reuse the same bulk sample indefinitely across control ticks.
        const uint32_t max_feedback_age = _feedback_bulk ? kStallFeedbackIntervalMs : 10;
        if (is_raw_pos_valid(_feedback_position) &&
            GetHAL().millis() - _feedback_tick <= max_feedback_age) {
            current_pos = _feedback_position;
            ++_read_shared;
        } else {
            current_pos = read_position_with_retry(communication_error);
        }
        if (!is_raw_pos_valid(current_pos)) _feedback_position = -1;
        bus_lock.unlock();
        if (!is_raw_pos_valid(current_pos)) {
            int fallback_angle = uitk::clamp(_profile == Profile::HardwareSweep && is_raw_pos_valid(_last_valid_position) ? raw_pos_to_angle(_last_valid_position) : Servo::getCurrentAngle(), getAngleLimit().x, getAngleLimit().y);
            mclog::tagWarn(_tag,
                           "id: {} ignore invalid current pos after retry: {}, error: {} ({}), fallback angle: {}, last_valid_age_ms: {}",
                           _config.id, current_pos, communication_error_name(communication_error),
                           static_cast<unsigned>(communication_error), fallback_angle, GetHAL().millis() - _last_valid_tick);
            return false;
        }

        sampled_at = _feedback_tick;
        angle = raw_pos_to_angle(current_pos);
        angle     = uitk::clamp(angle, getAngleLimit().x, getAngleLimit().y);
        // mclog::tagInfo(_tag, "id: {} current pos: {} angle: {}", _id, current_pos, angle);
        return true;
    }

    bool is_moving_impl() override
    {
        if (executor::external()) {
            bool result{};
            executor::invoke([&] { result = is_moving_impl(); });
            return result;
        }
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        int moving = _scs_bus.ReadMove(_config.id);
        // Preserve existing movement status semantics in this integration.
        return moving != 0;
    }

    void setTorqueEnabled(bool enabled) override
    {
        if (executor::external()) {
            executor::invoke([&] { setTorqueEnabled(enabled); });
            return;
        }
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        // Write ACK is optional and collected by the bus with a short timeout.
        if (_torque_command_known && _torque_commanded_enabled == enabled) return;
        if (!enabled && _profile == Profile::HardwareSweep) {
            // The hardware sweep owns a long target: retire it on torque release.
            uint8_t error = 0;
            const int raw = read_position_with_retry(error);
            if (is_raw_pos_valid(raw)) {
                _scs_bus.WritePos(_config.id, raw, 20, 0);
                stop_motion_at_angle(raw_pos_to_angle(raw));
            } else {
                stop_motion_at_angle(Servo::getCurrentAngle());
            }
            _sweep_hold_raw = -1;
        }
        _torque_command_known = true;
        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_ack_raw = -1;
        _scs_bus.EnableTorque(_config.id, enabled ? 1 : 0);
        if (!enabled) { _monitor_target = -1; reset_stall_detection(); }
        Servo::setTorqueEnabled(enabled);
    }

    bool getTorqueEnabled() override
    {
        if (executor::external()) {
            bool result{};
            executor::invoke([&] { result = getTorqueEnabled(); });
            return result;
        }
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        int torque_enable = _scs_bus.ReadToqueEnable(_config.id);
        // mclog::tagInfo(_tag, "id: {} torque enable: {}", _id, torque_enable);
        return torque_enable > 0;
    }

    void setCurrentAngleAsZero() override
    {
        // NVS writes remain on the caller's internal-stack context.
        std::lock_guard<std::mutex> calibration_lock(_calibration_mutex);
        int saved = -1;
        executor::invoke([&] {
            std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
            std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
            uint8_t communication_error = 0;
            const int current_pos = read_position_with_retry(communication_error);
            if (!is_raw_pos_valid(current_pos)) {
                mclog::tagWarn(_tag, "id: {} ignore invalid zero calibration pos: {}, error: {}",
                               _config.id, current_pos, communication_error_name(communication_error));
                return;
            }
            _zero_pos = current_pos;
            reset_runtime_limits();
            saved = _zero_pos;
        });
        if (saved < 0) return;
        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, saved);
        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, saved);
    }

    void resetZeroCalibration() override
    {
        std::lock_guard<std::mutex> calibration_lock(_calibration_mutex);
        executor::invoke([&] {
            std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
            _zero_pos = _config.defaultZeroPos;
            reset_runtime_limits();
        });
        Settings settings(_config.settingNs, true);
        settings.SetInt(_config.settingZeroPositionKey, _config.defaultZeroPos);
        mclog::tagInfo(_tag, "id: {} set zero pos: {} to settings", _config.id, _config.defaultZeroPos);
    }

    void rotate(int velocity) override
    {
        if (executor::external()) {
            executor::invoke([&] { rotate(velocity); });
            return;
        }
        std::lock_guard<std::recursive_mutex> state_lock(_state_mutex);
        std::lock_guard<std::mutex> bus_lock(_scs_bus_mutex);
        velocity = uitk::clamp(velocity, -1000, 1000);

        if (!_config.enablePwmMode) {
            return;
        }

        int mapped_velocity = map_range(velocity, 0, 1000, 0, 1023);

        // A manual PWM command supersedes a native position sweep. Retire
        // its profile and hold state before changing the actuator mode.
        if (_profile == Profile::HardwareSweep) {
            uint8_t error = 0;
            const int raw = read_position_with_retry(error);
            stop_motion_at_angle(is_raw_pos_valid(raw) ? raw_pos_to_angle(raw) : Servo::getCurrentAngle());
            _sweep_hold_raw = -1;
        }
        check_mode(Mode::PWM);
        _scs_bus.WritePWM(_config.id, mapped_velocity);
    }

private:
    enum class Mode { Position = 0, PWM = 1 };

    ServoConfig_t _config;
    std::mutex _calibration_mutex;
    Vector2i _runtime_raw_pos_limit;
    int _zero_pos      = 0;
    Mode _current_mode = Mode::Position;
    bool _torque_command_known = false;
    int _last_tracking_raw = -1;
    int _last_ack_raw = -1;
    int _last_ack_rate = -1;
    uint32_t _last_ack_revision = 0, _last_ack_tick = 0;
    uint32_t _normal_write_sent = 0, _normal_write_skipped = 0;
    uint32_t _tracking_write_sent = 0, _tracking_write_skipped = 0;
    static constexpr uint32_t kNormalRefreshMs = 250;
    int _monitor_target = -1;
    int _feedback_position = -1;
    int _last_valid_position = -1;
    uint32_t _last_valid_tick = 0;
    uint32_t _feedback_tick = 0;
    bool _feedback_bulk = false;
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
        if (is_raw_pos_valid(current_pos)) { ++_read_ok; cache_position(current_pos, false); return current_pos; }
        if (current_pos < 0 && communication_error != 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            current_pos = _scs_bus.ReadPos(_config.id);
            communication_error = _scs_bus.getLastError();
            if (is_raw_pos_valid(current_pos)) { ++_read_retry_ok; cache_position(current_pos, false); return current_pos; }
        }
        ++_read_fail;
        return current_pos;
    }

    void cache_position(int raw, bool bulk)
    {
        _feedback_position = raw;
        _feedback_tick = GetHAL().millis();
        _last_valid_position = raw;
        _last_valid_tick = _feedback_tick;
        _feedback_bulk = bulk;
    }

    void report_bus_stats()
    {
        const uint32_t now = GetHAL().millis();
        if (now - _diag_tick < 10000) return;
        _diag_tick = now;
        _scs_bus.reportDiagnostics();
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
        _last_valid_position = -1;
        _monitor_target = -1;
        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_ack_raw = -1;
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
            reset_stall_detection();
            return false;
        }

        const int current_raw_pos = _scs_bus.ReadPos(-1);
        const int current_abs     = abs_int(_scs_bus.ReadCurrent(-1));
        const int load_abs        = abs_int(_scs_bus.ReadLoad(-1));

        if (!is_raw_pos_valid(current_raw_pos)) {
            ++_stall_fail;
            ++_stall_errors[0];
            reset_stall_detection();
            return false;
        }
        ++_stall_ok;
        cache_position(current_raw_pos, true);
        // Do not compare stale samples after a long interruption.
        if (_feedback_tick - _stall_last_valid_tick > 150) reset_stall_detection();
        _stall_last_valid_tick = _feedback_tick;

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
            } else {
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
        _monitor_target = raw_pos;
        _feedback_position = -1;
        _last_tracking_raw = -1;
        _last_ack_raw = -1;

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
        _last_ack_raw = -1;
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
