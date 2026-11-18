/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include <mooncake_log.h>
#include <mcp_server.h>
#include <stackchan/stackchan.h>
#include <stackchan/port_stage1/port_stage1.h>
#include <stackchan/port_stage1/quiet_mode.h>
#include <apps/common/common.h>
#include <cstdlib>
#include <ctime>

using namespace stackchan;

static const std::string_view _tag = "HAL-MCP";

void Hal::xiaozhi_mcp_init()
{
    mclog::tagInfo(_tag, "init");

    // https://github.com/78/xiaozhi-esp32/blob/main/docs/mcp-usage.md
    auto& mcp_server = McpServer::GetInstance();

    // System Prompt：
    // You can control the robot's head. Use get_yaw and get_pitch to sense current position. Use set_yaw for horizontal
    // movement and set_pitch for vertical movement. All angles are in degrees.

    mclog::tagInfo(_tag, "add system.get_local_time tool");
    mcp_server.AddTool(
        "self.system.get_local_time",
        "Returns the current date, time, and weekday from this device using its configured timezone. "
        "You MUST call this tool whenever the user asks for the current time, date, weekday, or a relative date. "
        "Do not infer the answer from server time.",
        std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
            // Reapply the saved POSIX timezone here so this tool does not depend on
            // initialization order or on another component changing the process TZ.
            std::string timezone = getTimezone();
            setenv("TZ", timezone.c_str(), 1);
            tzset();

            std::time_t now = std::time(nullptr);
            if (now < 1577836800) {
                return std::string(R"({"synchronized":false,"error":"Device clock is not synchronized yet."})");
            }

            struct tm local_time = {};
            if (localtime_r(&now, &local_time) == nullptr) {
                return std::string(R"({"synchronized":false,"error":"Failed to read device local time."})");
            }

            char date_time[32] = {};
            char weekday[16]   = {};
            char zone_name[16] = {};
            char offset_raw[8] = {};
            if (std::strftime(date_time, sizeof(date_time), "%Y-%m-%d %H:%M:%S", &local_time) == 0 ||
                std::strftime(weekday, sizeof(weekday), "%A", &local_time) == 0 ||
                std::strftime(zone_name, sizeof(zone_name), "%Z", &local_time) == 0) {
                return std::string(R"({"synchronized":false,"error":"Failed to format device local time."})");
            }
            std::strftime(offset_raw, sizeof(offset_raw), "%z", &local_time);

            std::string utc_offset = offset_raw;
            if (utc_offset.size() == 5) {
                utc_offset.insert(3, ":");
            }
            if (timezone == "JST-9") {
                // POSIX reverses the offset sign in "JST-9". Return the familiar
                // notation as well so the language model cannot mistake it for UTC-9.
                utc_offset = "+09:00";
            }

            auto result = fmt::format(
                R"({{"synchronized":true,"local_datetime":"{}","weekday":"{}","timezone_abbreviation":"{}","utc_offset":"{}","timezone_posix":"{}","unix_time":{},"instruction":"Use local_datetime exactly as the device local time. It is already converted from UTC; do not convert it again or answer with UTC."}})",
                date_time, weekday, zone_name, utc_offset, timezone, static_cast<long long>(now));
            mclog::tagInfo(_tag, "get_local_time: {}", result);
            return result;
        });

    mclog::tagInfo(_tag, "add robot.get_head_angles tool");
    mcp_server.AddTool("self.robot.get_head_angles",
                       "Returns current yaw/pitch in degrees. Neutral position is {yaw:0, pitch:0}.",
                       std::vector<Property>{}, [this](const PropertyList& properties) -> ReturnValue {
                           LvglLockGuard lock;  // StackChan motion update is under the lvgl lock

                           auto& motion      = GetStackChan().motion();
                           int current_yaw   = motion.yawServo().getCurrentAngle() / 10;
                           int current_pitch = motion.pitchServo().getCurrentAngle() / 10;

                           auto result = fmt::format(R"({{"yaw": {}, "pitch": {}}})", current_yaw, current_pitch);
                           mclog::tagInfo(_tag, "get_head_angles: {}", result);
                           return result;
                       });

    mclog::tagInfo(_tag, "add robot.set_head_angles tool");
    mcp_server.AddTool("self.robot.set_head_angles",
                       "Adjust head position. GUIDELINES: "
                       "1. For natural interaction, stay within +/- 45 degrees. "
                       "2. Only use values > 70 if the user explicitly asks to look far away/behind. "
                       "3. Max ranges: Yaw(-128 to 128, -128 as your left), Pitch(0 to 90, 90 as your up). "
                       "Speed(100-1000, 150 is natural).",
                       PropertyList({Property("yaw", kPropertyTypeInteger, -9999, -9999, 128),
                                     Property("pitch", kPropertyTypeInteger, -9999, -9999, 90),
                                     Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int speed = properties["speed"].value<int>();
                           int yaw   = properties["yaw"].value<int>();
                           int pitch = properties["pitch"].value<int>();

                           mclog::tagInfo(_tag, "motion set_angles: yaw: {}, pitch: {}, speed: {}", yaw, pitch, speed);

                           LvglLockGuard lock;

                           stackchan::stage1::CancelForExternalChange(GetStackChan());
                           stackchan::stage1::NotifyManualMotion();
                           auto& motion = GetStackChan().motion();
                           if (pitch != -9999) {
                               motion.pitchServo().moveWithSpeed(pitch * 10, speed);
                           }
                           if (yaw != -9999) {
                               motion.yawServo().moveWithSpeed(yaw * 10, speed);
                           }

                           return true;
                       });

    // Relative voice head movement: read the servo's actual current angle
    // and its live limits under the same LVGL lock before commanding motion.
    // The servo's runtime limits may shrink after stall protection triggers.
    mclog::tagInfo(_tag, "add robot.adjust_head_angles tool");
    mcp_server.AddTool(
        "self.robot.adjust_head_angles",
        "Move the head relative to its current position by a bounded 10-degree step. "
        "yaw_delta: +10 right, -10 left; pitch_delta: +10 up, -10 down. "
        "Each axis is clamped to the servo's current limits, including stall-protection limits. "
        "Only one axis may be moved per call.",
        PropertyList({Property("yaw_delta", kPropertyTypeInteger, 0, -10, 10),
                      Property("pitch_delta", kPropertyTypeInteger, 0, -10, 10),
                      Property("speed", kPropertyTypeInteger, 150, 100, 1000)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int yaw_delta = properties["yaw_delta"].value<int>();
            const int pitch_delta = properties["pitch_delta"].value<int>();
            const int speed = properties["speed"].value<int>();

            // Reject non-10-degree increments and combined movement.
            if ((yaw_delta != 0 && yaw_delta != 10 && yaw_delta != -10) ||
                (pitch_delta != 0 && pitch_delta != 10 && pitch_delta != -10) ||
                ((yaw_delta == 0) == (pitch_delta == 0))) {
                return false;
            }

            LvglLockGuard lock;
            auto& motion = GetStackChan().motion();
            stackchan::stage1::CancelForExternalChange(GetStackChan());
            stackchan::stage1::NotifyManualMotion();

            if (yaw_delta != 0) {
                auto& servo = motion.yawServo();
                const auto limit = servo.getAngleLimit();
                const int before = servo.getCurrentAngle();
                const int target = uitk::clamp(before + yaw_delta * 10, limit.x, limit.y);
                mclog::tagInfo(_tag, "relative yaw: {} -> {}, limits: {}..{}", before, target, limit.x, limit.y);
                if (target != before) servo.moveWithSpeed(target, speed);
            } else {
                auto& servo = motion.pitchServo();
                const auto limit = servo.getAngleLimit();
                const int before = servo.getCurrentAngle();
                const int target = uitk::clamp(before + pitch_delta * 10, limit.x, limit.y);
                mclog::tagInfo(_tag, "relative pitch: {} -> {}, limits: {}..{}", before, target, limit.x, limit.y);
                if (target != before) servo.moveWithSpeed(target, speed);
            }
            return true;
        });

    // Reuse the stage-1 action sequencer; MCP workers never touch servo/LVGL.
    // Only an explicit requested action is allowed to move the head.
    mclog::tagInfo(_tag, "add robot.play_action tool");
    mcp_server.AddTool(
        "self.robot.play_action",
        "Play one short, predefined head gesture ONLY when the user explicitly "
        "requests it. Valid action IDs: nod (うなずく), shake_head (首を横に振る), "
        "tilt_head (首を傾ける), happy_move (喜ぶ), surprise_move (驚く), "
        "shy_move (照れる). No arbitrary angles. "
        "Returns false if another scene or action has priority. "
        "Do not repeat automatically.",
        PropertyList({Property("action", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& properties) -> ReturnValue {
            const auto action = properties["action"].value<std::string>();
            const bool queued = stackchan::stage1::RequestCue(action.c_str());
            mclog::tagInfo(_tag, "play_action: action={}, queued={}", action, queued);
            return queued;
        });

    mclog::tagInfo(_tag, "add robot.set_led_color tool");
    mcp_server.AddTool(
        "self.robot.set_led_color",
        "Set the color of the robot's INTERNAL onboard LED. This is NOT for room lights. "
        "Values: 0-168 (safe range). Red=168,0,0; Green=0,168,0; Blue=0,0,168; White=100,100,100; Off=0,0,0.",
        PropertyList({Property("red", kPropertyTypeInteger, 0, 0, 168),
                      Property("green", kPropertyTypeInteger, 0, 0, 168),
                      Property("blue", kPropertyTypeInteger, 0, 0, 168)}),
        [this](const PropertyList& properties) -> ReturnValue {
            int r = properties["red"].value<int>();
            int g = properties["green"].value<int>();
            int b = properties["blue"].value<int>();

            mclog::tagInfo(_tag, "set_led_color: r={}, g={}, b={}", r, g, b);

            LvglLockGuard lock;

            stackchan::stage1::CancelForExternalChange(GetStackChan());
            stackchan::stage1::NotifyManualLed();
            GetStackChan().leftNeonLight().setColor(r, g, b);
            GetStackChan().rightNeonLight().setColor(r, g, b);

            return true;
        });

    mclog::tagInfo(_tag, "add robot.set_quiet_mode tool");
    mcp_server.AddTool(
        "self.robot.set_quiet_mode",
        "Silence autonomous speech, sleep talk and snoring temporarily. "
        "Does not disable manual conversation, wake words, or Web tests. "
        "minutes=0 cancels; 1..1440 schedules a temporary quiet period. "
        "Only call on an explicit user request.",
        PropertyList({Property("minutes", kPropertyTypeInteger, 30, 0, 1440)}),
        [](const PropertyList& properties) -> ReturnValue {
            const int minutes = properties["minutes"].value<int>();
            const bool saved = stackchan::stage1::quiet_mode::SetMinutes(minutes);
            mclog::tagInfo(_tag, "set_quiet_mode: minutes={} saved={}", minutes, saved);
            return saved;
        });

    // Voice relative volume control complements the existing
    // self.audio_speaker.set_volume absolute tool. Use the same HAL entry
    // point as Web SETUP with permanent=true for persistent settings.
    mclog::tagInfo(_tag, "add robot.adjust_volume tool");
    mcp_server.AddTool(
        "self.robot.adjust_volume",
        "Increase or decrease this robot's speaker volume in percent. "
        "delta must be +10 or -10. This is for explicit voice volume requests; "
        "never use it for quoted, negative, or hypothetical instructions.",
        PropertyList({Property("delta", kPropertyTypeInteger, 10, -10, 10)}),
        [this](const PropertyList& properties) -> ReturnValue {
            const int delta = properties["delta"].value<int>();
            if (delta != 10 && delta != -10) return false;

            const int current = static_cast<int>(getSpeakerVolume());
            int next = current + delta;
            if (next < 0) next = 0;
            if (next > 100) next = 100;
            setSpeakerVolume(static_cast<uint8_t>(next), true);
            const int applied = static_cast<int>(getSpeakerVolume());
            mclog::tagInfo(_tag, "adjust_volume: from={} delta={} applied={}",
                           current, delta, applied);
            return applied == next;
        });

    mclog::tagInfo(_tag, "add robot.create_reminder tool");
    mcp_server.AddTool("self.robot.create_reminder",
                       "Create a reminder. Duration is in seconds. Message is what to say when time is up. Set repeat "
                       "to true to repeat the reminder.",
                       PropertyList({Property("duration_seconds", kPropertyTypeInteger, 60, 1, 86400),
                                     Property("message", kPropertyTypeString, std::string("Time's up!")),
                                     Property("repeat", kPropertyTypeBoolean, false)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int duration_seconds = properties["duration_seconds"].value<int>();
                           std::string message  = properties["message"].value<std::string>();
                           bool repeat          = properties["repeat"].value<bool>();

                           // Default message
                           if (message.empty()) {
                               message = "Time's up!";
                           }

                           mclog::tagInfo(_tag, "create_reminder: duration={}s, message={}, repeat={}",
                                          duration_seconds, message, repeat);

                           int id = tools::create_reminder(duration_seconds * 1000, message, repeat);

                           return id;
                       });

    mclog::tagInfo(_tag, "add robot.get_reminders tool");
    mcp_server.AddTool("self.robot.get_reminders", "Get list of active reminders.", std::vector<Property>{},
                       [this](const PropertyList& properties) -> ReturnValue {
                           mclog::tagInfo(_tag, "get_reminders");
                           auto reminders          = tools::get_active_reminders();
                           std::string result_json = "[";
                           for (size_t i = 0; i < reminders.size(); ++i) {
                               const auto& r = reminders[i];
                               result_json +=
                                   fmt::format(R"({{"id": {}, "duration_ms": {}, "message": "{}", "repeat": {}}})",
                                               r.id, r.durationMs, r.message, r.repeat ? "true" : "false");
                               if (i < reminders.size() - 1) {
                                   result_json += ", ";
                               }
                           }
                           result_json += "]";
                           mclog::tagInfo(_tag, "get_reminders result: {}", result_json);
                           return result_json;
                       });

    mclog::tagInfo(_tag, "add robot.stop_reminder tool");
    mcp_server.AddTool("self.robot.stop_reminder", "Stop a reminder by ID.",
                       PropertyList({Property("id", kPropertyTypeInteger, -1)}),
                       [this](const PropertyList& properties) -> ReturnValue {
                           int id = properties["id"].value<int>();
                           mclog::tagInfo(_tag, "stop_reminder: id={}", id);
                           tools::stop_reminder(id);
                           return true;
                       });
}
