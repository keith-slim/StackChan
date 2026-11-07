/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "default.h"
#include <algorithm>
#include "../../speech_text_layout.h"

using namespace uitk;
using namespace uitk::lvgl_cpp;
using namespace stackchan::avatar;

static const Vector2i _container_pos  = Vector2i(0, 89);
static const Vector2i _container_size = Vector2i(320, 74);
static const Vector2i _arrow_offset   = Vector2i(40, -15);
static const int _text_mx             = 20;
static const int _bubble_min_width    = 90;
static const int _bubble_max_width    = 340;
static const int _bubble_height       = 52;
static const int _bubble_min_offset_x = 66;
static const int _bubble_max_offset_x = 0;

LV_IMAGE_DECLARE(default_bubble_arrow);

DefaultSpeechBubble::DefaultSpeechBubble(lv_obj_t* parent, lv_color_t primaryColor, lv_color_t secondaryColor,
                                         const lv_font_t* font)
{
    _container = std::make_unique<Container>(parent);
    _container->setRadius(0);
    _container->setAlign(LV_ALIGN_CENTER);
    _container->setBorderWidth(0);
    _container->setBgOpa(0);
    _container->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _container->setSize(_container_size.x, _container_size.y);
    _container->setPos(_container_pos.x, _container_pos.y);
    _container->setPadding(0, 0, 0, 0);

    _arrow = std::make_unique<Image>(_container->get());
    _arrow->setSrc(&default_bubble_arrow);
    _arrow->setAlign(LV_ALIGN_CENTER);
    _arrow->setPos(_arrow_offset.x, _arrow_offset.y);
    _arrow->setImageRecolorOpa(LV_OPA_COVER);
    _arrow->setImageRecolor(primaryColor);

    _bubble = std::make_unique<Container>(_container->get());
    _bubble->setRadius(LV_RADIUS_CIRCLE);
    _bubble->setAlign(LV_ALIGN_CENTER);
    _bubble->setBorderWidth(0);
    _bubble->setBgColor(primaryColor);
    _bubble->removeFlag(LV_OBJ_FLAG_SCROLLABLE);
    _bubble->setSize(_bubble_max_width, _bubble_height);
    _bubble->setPos(0, 11);

    _text = std::make_unique<Label>(_bubble->get());
    _text->setTextColor(secondaryColor);
    _text->setTextFont(font);
    _text->setTextAlign(LV_TEXT_ALIGN_CENTER);
    _text->setAlign(LV_ALIGN_CENTER);
    _text->setPos(0, 0);
    _text->setWidth(320 - _text_mx * 2);
    // スクロールの時間を15秒に設定
    lv_obj_set_style_anim_duration(_text->get(), 4500, LV_PART_MAIN);
    _text->setLongMode(LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    clearSpeech();
}

DefaultSpeechBubble::~DefaultSpeechBubble()
{
    _text.reset();
    _bubble.reset();
    _arrow.reset();
    _container.reset();
}




namespace {
int subtitle_glyph_width(uint32_t ch, uint32_t next, void* context)
{
    const auto* font = static_cast<const lv_font_t*>(context);
    return std::max(lv_font_get_glyph_width(font, ch, next),
                    lv_font_get_glyph_width(font, ch, 0));
}
}

std::vector<std::string> DefaultSpeechBubble::splitSubtitle(std::string_view text) const
{
    auto unsplit = [&]() {
        return std::vector<std::string>{std::string(text)};
    };
    if (text.empty() || text.size() > speech_layout::MAX_TEXT_BYTES) return unsplit();

    const auto* font = _text->getTextFont();
    const int max_width = _bubble_max_width - _text_mx * 2;
    const size_t pos = speech_layout::FindTwoLinePhraseSplit(
        text.data(), text.size(), max_width, subtitle_glyph_width,
        const_cast<lv_font_t*>(font));
    if (pos == 0 || pos >= text.size()) return unsplit();

    const int left_width = speech_layout::MeasureWidth(
        text.data(), pos, subtitle_glyph_width, const_cast<lv_font_t*>(font));
    const int right_width = speech_layout::MeasureWidth(
        text.data() + pos, text.size() - pos, subtitle_glyph_width,
        const_cast<lv_font_t*>(font));
    if (left_width <= 0 || right_width <= 0) return unsplit();

    auto read_ms = [](int width) -> uint32_t {
        const uint32_t ms = 800u + static_cast<uint32_t>(width) * 7u;
        return std::clamp<uint32_t>(ms, 1700u, 4000u);
    };
    if (read_ms(left_width) + read_ms(right_width) > 5500u) return unsplit();

    return {std::string(text.substr(0, pos)), std::string(text.substr(pos))};
}

void DefaultSpeechBubble::setSpeech(std::string_view text)
{
    if (text.empty()) {
        clearSpeech();
        return;
    }

    // 新しい文章を設定
    _text->setText(text);

    // 実際のフォントで横幅を測定
    lv_point_t text_size = {};
    lv_text_get_size(
        &text_size,
        text.data(),
        _text->getTextFont(),
        0, 0,
        LV_COORD_MAX,
        LV_TEXT_FLAG_NONE
    );

    // 元の可変幅の吹き出し
    int bubble_width =
        min(text_size.x + _text_mx * 2, _bubble_max_width);

    bubble_width =
        max(bubble_width, _bubble_min_width);

    auto bubble_offset_x =
        map_range(
            bubble_width,
            _bubble_min_width,
            _bubble_max_width,
            _bubble_min_offset_x,
            _bubble_max_offset_x
        );

    _bubble->setWidth(bubble_width);
    _bubble->setX(bubble_offset_x);

    // 文字の表示領域を吹き出しの内幅に合わせる
    const int32_t viewport_width =
        bubble_width - _text_mx * 2;

    _text->setWidth(viewport_width);

    // 実際の表示領域を基準に表示時間を計算
    if (text_size.x > viewport_width) {
        _display_duration_ms = 5500;
    } else {
        const uint32_t ms =
            800u + static_cast<uint32_t>(
                std::max<int32_t>(0, text_size.x)) * 7u;

        _display_duration_ms =
            std::clamp<uint32_t>(ms, 1700u, 4000u);
    }

    // 前のスクロール位置をリセット
    _text->setLongMode(
        LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
    );

    setVisible(true);
}

void DefaultSpeechBubble::clearSpeech()
{
    _text->setText("");
    setVisible(false);
}

void DefaultSpeechBubble::setVisible(bool visible)
{
    SpeechBubble::setVisible(visible);

    _container->setHidden(!visible);
}

void DefaultSpeechBubble::setTextFont(void* font)
{
    if (_text && font) {
        _text->setTextFont((lv_font_t*)font);
    }
}
