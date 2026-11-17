#pragma once
#include "avatar_config.h"
#include "lvgl.h"
#include <algorithm>

namespace stackchan::avatar {
// 全画面の中間画像を作らず、部品の寸法を直接拡大してメモリを抑える。
inline int viewport_scale(lv_obj_t *object)
{
    auto *display = lv_obj_get_display(object);
    return std::min(lv_display_get_horizontal_resolution(display) * avatar_config::SCALE_UNIT /
                        avatar_config::REFERENCE_WIDTH,
                    lv_display_get_vertical_resolution(display) * avatar_config::SCALE_UNIT /
                        avatar_config::REFERENCE_HEIGHT);
}
// 負座標も正座標と同じ向きに丸め、左右の部品を対称に配置する。
inline int scaled(int value, int scale)
{
    return (value * scale + (value < 0 ? -avatar_config::SCALE_UNIT / 2 : avatar_config::SCALE_UNIT / 2)) /
           avatar_config::SCALE_UNIT;
}
} // namespace stackchan::avatar
