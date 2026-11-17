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
    case Emotion::Sad: {
        auto left = std::make_unique<SweatDecorator>(panel);
        left->setPosition(-70, 10);
        _s3_decoration_one = std::move(left);
        auto right = std::make_unique<SweatDecorator>(panel);
        right->setPosition(70, 10);
        _s3_decoration_two = std::move(right);
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
