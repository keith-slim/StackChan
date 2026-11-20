/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

// Shared policy for reconciling the normal idle modifiers after a status
// callback or an audio-state watchdog tick. This has no timing, LVGL, or
// hardware dependencies; sleep/wake/theatre retain Stage1 ownership.
namespace stackchan::idle_lifecycle {

struct State {
    bool device_idle;
    bool announcement_active;
    bool scene_active;
    bool conversation_active;
    bool wake_active;
    bool explicit_sleep;
};

inline bool AudioFinished(const State& state)
{
    return state.device_idle && !state.announcement_active;
}

inline bool AllowIdleModifiers(const State& state)
{
    return AudioFinished(state) && !state.scene_active &&
           !state.conversation_active && !state.wake_active &&
           !state.explicit_sleep;
}

} // namespace stackchan::idle_lifecycle
