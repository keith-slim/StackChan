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

// Add future idle/scene/decoration decisions here rather than duplicating
// state checks in normal status callbacks and missed-STANDBY recovery.
struct Decision {
    bool audio_finished;
    bool restore_conversation_face;
    bool allow_idle_modifiers;
};

inline Decision Resolve(const State& state)
{
    const bool finished = state.device_idle && !state.announcement_active;
    const bool restore_conversation =
        finished && state.conversation_active &&
        !state.scene_active && !state.wake_active;
    return {finished, restore_conversation,
            finished && !state.scene_active && !state.conversation_active &&
            !state.wake_active && !state.explicit_sleep};
}

} // namespace stackchan::idle_lifecycle
