/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "../modifiable.h"
#include "../port_stage1/port_stage1.h"
#include <hal/hal.h>
#include <cstdint>
#include <atomic>

namespace stackchan {

/**
 * @brief
 *
 */
class ImuEventModifier : public Modifier {
public:
    ImuEventModifier(uint32_t reactionDurationMs = 4000) : _reaction_duration_ms(reactionDurationMs)
    {
        _signal_connection = GetHAL().onImuMotionEvent.connect([this](ImuMotionEvent event) {
            if (event == ImuMotionEvent::Shake) {
                _event_shake = true;
            }
        });
    }

    ~ImuEventModifier()
    {
        GetHAL().onImuMotionEvent.disconnect(_signal_connection);
    }

    // Restore only what the shake changed, before another face owner captures
    // its baseline. The pending shake must not replay after the handoff.
    void onFaceTakeover(Modifiable& stackchan) override
    {
        _event_shake.store(false);
        if (_is_reacting) restore_state(stackchan);
    }

    void _update(Modifiable& stackchan) override
    {
        // Sleep/skits/conversation take priority over the legacy shake
        // reaction. Clear a pre-existing reaction as well as new events;
        // otherwise it can keep rotating the mouth and command goHome().
        if (stage1::IsAutonomousSceneActive() || stage1::IsConversationFaceActive()) {
            _event_shake.store(false);
            if (_is_reacting) restore_state(stackchan);
            return;
        }
        uint32_t now = GetHAL().millis();

        // 收到晃动事件
        if (_event_shake.exchange(false) &&
            (_is_reacting || !stackchan.avatar().isModifyLocked())) {
            handle_shake_start(stackchan, now);
        }

        // 如果处于晃动反应状态
        if (_is_reacting) {
            if (now >= _next_toggle_tick) {
                _next_toggle_tick = now + 600;

                _toggle_phase      = !_toggle_phase;
                int mouth_rotation = _toggle_phase ? -25 : 25;

                auto& avatar = stackchan.avatar();
                avatar.mouth().setRotation(mouth_rotation);
                avatar.mouth().setWeight(65);
            }

            //  Lock motion modify and move home
            auto& motion = stackchan.motion();
            if (!motion.isModifyLocked()) {
                motion.setModifyLock(true);
                _motion_lock_owned = true;
                motion.goHome(300);
            }

            // 检查是否结束反应
            if (now >= _restore_at) {
                restore_state(stackchan);
            }
        }
    }

private:
    void handle_shake_start(Modifiable& stackchan, uint32_t now)
    {
        if (!_is_reacting) {
            // 首次触发时，记录状态以便恢复
            _is_reacting = true;

            auto& avatar = stackchan.avatar();

            _saved_left_visible = avatar.leftEye().getVisible();
            _saved_right_visible = avatar.rightEye().getVisible();
            _saved_mouth_weight = avatar.mouth().getWeight();
            _saved_mouth_rotation = avatar.mouth().getRotation();
            avatar.setModifyLock(true);
            _avatar_lock_owned = true;
            avatar.leftEye().setVisible(false);
            avatar.rightEye().setVisible(false);

            stackchan.avatar().removeDecorator(_dizzy_decorator_id);
            stackchan.avatar().removeDecorator(_shy_decorator_id);
            _dizzy_decorator_id =
                stackchan.avatar().addDecorator(std::make_unique<avatar::DizzyDecorator>(lv_screen_active(), 0, 300));
            _shy_decorator_id =
                stackchan.avatar().addDecorator(std::make_unique<avatar::ShyDecorator>(lv_screen_active(), 0));
        }

        // 刷新恢复时间和切换时间
        _restore_at = now + _reaction_duration_ms;
        if (_next_toggle_tick <= now) {
            _next_toggle_tick = now;  // 立即触发第一次嘴巴动作
        }
    }

    void restore_state(Modifiable& stackchan)
    {
        if (!_is_reacting) {
            return;
        }

        auto& avatar = stackchan.avatar();
        if (_avatar_lock_owned) avatar.setModifyLock(false);
        _avatar_lock_owned = false;
        avatar.leftEye().setVisible(_saved_left_visible);
        avatar.rightEye().setVisible(_saved_right_visible);
        avatar.mouth().setWeight(_saved_mouth_weight);
        avatar.mouth().setRotation(_saved_mouth_rotation);

        stackchan.avatar().removeDecorator(_dizzy_decorator_id);
        stackchan.avatar().removeDecorator(_shy_decorator_id);
        _dizzy_decorator_id = -1;
        _shy_decorator_id   = -1;

        auto& motion = stackchan.motion();
        if (_motion_lock_owned) motion.setModifyLock(false);
        _motion_lock_owned = false;
        _is_reacting = false;
    }

    // 信号相关
    int _signal_connection;
    std::atomic_bool _event_shake{false};

    // 状态控制
    bool _is_reacting          = false;
    bool _avatar_lock_owned = false;
    bool _motion_lock_owned = false;
    bool _saved_left_visible = true;
    bool _saved_right_visible = true;
    int _saved_mouth_weight = 0;
    int _saved_mouth_rotation = 0;
    bool _toggle_phase         = false;
    uint32_t _restore_at       = 0;
    uint32_t _next_toggle_tick = 0;
    uint32_t _reaction_duration_ms;

    int _dizzy_decorator_id = -1;
    int _shy_decorator_id   = -1;
};

}  // namespace stackchan
