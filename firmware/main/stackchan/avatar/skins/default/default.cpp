/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"
#include "../../decorators/decorators.h"

using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

void DefaultAvatar::init(lv_obj_t* parent, const lv_font_t* font)
{
    _pannel = std::make_unique<Container>(parent);
    _pannel->align(LV_ALIGN_CENTER, 0, 0);
    _pannel->setSize(320, 240);
    _pannel->setRadius(0);
    _pannel->setBorderWidth(0);
    _pannel->setBgColor(secondaryColor);
    _pannel->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    _key_elements.leftEye  = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, true);
    _key_elements.rightEye = std::make_unique<DefaultEyes>(_pannel->get(), primaryColor, secondaryColor, false);
    _key_elements.mouth    = std::make_unique<DefaultMouth>(_pannel->get(), primaryColor, secondaryColor);
    _key_elements.speechBubble =
        std::make_unique<DefaultSpeechBubble>(_pannel->get(), primaryColor, secondaryColor, font);
}

// S3 AvatarBehavior::ApplyPose() decoration rules, adapted to the original
// StackChan avatar without clearing pet/IMU decorators or creating new tasks.
void DefaultAvatar::setEmotion(const Emotion& emotion)
{
    if (getEmotion() == emotion) {
        Avatar::setEmotion(emotion);
        return;
    }
    if (getEmotion() == Emotion::LowBattery) {
        for (auto& line : _low_battery_lines) {
            if (line) lv_obj_delete(line);
            line = nullptr;
        }
        leftEye().setVisible(_low_eye_visible[0]);
        rightEye().setVisible(_low_eye_visible[1]);
        mouth().setVisible(_low_mouth_visible);
    }
    _s3_decoration_two.reset();
    _s3_decoration_one.reset();
    Avatar::setEmotion(emotion);
    if (!_pannel) return;

    auto* panel = _pannel->get();
    switch (emotion) {
    case Emotion::Angry:
        _s3_decoration_one = std::make_unique<AngryDecorator>(panel);
        break;
    case Emotion::Happy:
        _s3_decoration_one = std::make_unique<ShyDecorator>(panel);
        break;
    case Emotion::LowBattery:
    case Emotion::Sad: {
        auto left = std::make_unique<SweatDecorator>(panel);
        left->setPosition(-70, 10);
        _s3_decoration_one = std::move(left);
        auto right = std::make_unique<SweatDecorator>(panel);
        right->setPosition(70, 10);
        _s3_decoration_two = std::move(right);
        if (emotion == Emotion::LowBattery) {
            // The approved browless face: closed curved eyes and frown.
            // Existing SweatDecorator instances above animate both tears.
            _low_eye_visible[0] = leftEye().getVisible();
            _low_eye_visible[1] = rightEye().getVisible();
            _low_mouth_visible = mouth().getVisible();
            leftEye().setVisible(false);
            rightEye().setVisible(false);
            mouth().setVisible(false);
            static const lv_point_precise_t eye[] = {{0,0},{5,5},{10,8},{15,10},{20,11},{25,10},{30,8},{35,5},{40,0}};
            static const lv_point_precise_t frown[] = {{0,10},{5,5},{10,2},{15,0},{20,0},{25,2},{30,5},{35,10}};
            for (int i = 0; i < 3; ++i) {
                auto* line = lv_line_create(panel);
                _low_battery_lines[i] = line;
                lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_style_line_color(line, primaryColor, 0);
                lv_obj_set_style_line_width(line, 5, 0);
                lv_obj_set_style_line_rounded(line, true, 0);
                lv_line_set_points(line, i == 2 ? frown : eye, i == 2 ? 8 : 9);
                lv_obj_align(line, LV_ALIGN_CENTER, i == 0 ? -70 : i == 1 ? 70 : 0,
                             i == 2 ? 26 : -17);
            }
        }
        break;
    }
    case Emotion::Fun:
        _s3_decoration_one = std::make_unique<HeartDecorator>(panel);
        break;
    default:
        break;
    }
}

void DefaultAvatar::update()
{
    Avatar::update();
    if (getEmotion() == Emotion::LowBattery && _low_battery_lines[2]) {
        const bool open = mouth().getWeight() > 3;
        if (mouth().getVisible() != open) mouth().setVisible(open);
        if (open) lv_obj_add_flag(_low_battery_lines[2], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(_low_battery_lines[2], LV_OBJ_FLAG_HIDDEN);
    }
    if (_s3_decoration_one) _s3_decoration_one->_update();
    if (_s3_decoration_two) _s3_decoration_two->_update();
}

Container* DefaultAvatar::getPanel() const
{
    if (_pannel) {
        return _pannel.get();
    }
    return NULL;
}
