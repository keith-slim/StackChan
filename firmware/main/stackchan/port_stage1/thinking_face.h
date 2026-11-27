// S3-stackchan's AvatarThinkingFace, adapted to StackChan's existing LVGL
// compatibility layer. Header-only to avoid adding a new task or build unit.
#pragma once
#include "port_compat.h"
#include "avatar_viewport.h"
#include <algorithm>
#include <cmath>
#include "lvgl.h"
#include <cstdint>

// 考え顔と装飾を必要な間だけ所有し、全画面画像や追加の描画バッファを持たない。
class AvatarThinkingFace {
public:
    explicit AvatarThinkingFace(lv_obj_t *parent);
    ~AvatarThinkingFace();
    void Render(bool speaking, uint8_t mouth, uint32_t elapsed_ms);
private:
    lv_obj_t *root_, *eyes_[2], *mouth_, *cloud_, *links_[2], *dots_[3];
    lv_point_precise_t cloud_points_[49] = {};
    int scale_;
};

inline AvatarThinkingFace::AvatarThinkingFace(lv_obj_t *parent)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_clickable(root_, false);
    lv_obj_set_scrollable(root_, false);
    scale_ = stackchan::avatar::viewport_scale(parent);
    const auto px = [this](int n) { return stackchan::avatar::scaled(n, scale_); };
    const auto shape = [this]() {
        auto *object = lv_obj_create(root_);
        lv_obj_remove_style_all(object);
        lv_obj_set_clickable(object, false);
        lv_obj_set_scrollable(object, false);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(object, lv_color_white(), 0);
        lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
        return object;
    };
    for (auto &eye : eyes_) eye = shape();
    mouth_ = shape();
    cloud_ = lv_line_create(root_);
    lv_obj_set_style_line_color(cloud_, lv_color_white(), 0);
    lv_obj_set_style_line_width(cloud_, std::max(1, px(2)), 0);
    lv_obj_set_style_line_rounded(cloud_, true, 0);
    // 承認された配置案の大きさへ等倍率で合わせ、全画面画像を持たず固定長の点列で描く。
    constexpr float cloud_scale = 0.78f;
    constexpr float curves[][8] = {
        {-30,14, -46,12, -46,-9, -29,-12},
        {-29,-12, -29,-30, -6,-37, 8,-24},
        {8,-24, 23,-31, 40,-22, 37,-9},
        {37,-9, 54,-4, 51,17, 35,19},
        {35,19, 29,34, 7,34, -2,25},
        {-2,25, -14,33, -35,29, -30,14},
    };
    unsigned index = 0;
    for (const auto &c : curves) for (int step = 0; step < 8; ++step) {
        const float t = step / 8.0f, u = 1 - t;
        const float x = u*u*u*c[0] + 3*u*u*t*c[2] + 3*u*t*t*c[4] + t*t*t*c[6];
        const float y = u*u*u*c[1] + 3*u*u*t*c[3] + 3*u*t*t*c[5] + t*t*t*c[7];
        cloud_points_[index++] = {
            static_cast<lv_value_precise_t>(std::lround((x+46)*cloud_scale*scale_/avatar_config::SCALE_UNIT)),
            static_cast<lv_value_precise_t>(std::lround((y+37)*cloud_scale*scale_/avatar_config::SCALE_UNIT))};
    }
    cloud_points_[48] = cloud_points_[0];
    lv_line_set_points(cloud_, cloud_points_, 49);
    for (auto &link : links_) link = shape();
    for (auto &dot : dots_) dot = shape();
    // 右上の雲と接続の丸を右目から離す。縦横回転・会話中・スワイプ表示で同じ配置を使う。
    lv_obj_align(cloud_, LV_ALIGN_CENTER, px(104), px(-83));
    for (int i = 0; i < 2; ++i) {
        lv_obj_set_size(links_[i], px(i ? 6 : 10), px(i ? 6 : 10));
        lv_obj_align(links_[i], LV_ALIGN_CENTER, px(i ? 70 : 80), px(i ? -44 : -52));
    }
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_size(dots_[i], px(7), px(7));
        lv_obj_align(dots_[i], LV_ALIGN_CENTER, px(91+i*13), px(-80));
    }
}

inline AvatarThinkingFace::~AvatarThinkingFace() { lv_obj_delete(root_); }

inline void AvatarThinkingFace::Render(bool speaking, uint8_t mouth, uint32_t elapsed_ms)
{
    const auto px = [this](int n) { return stackchan::avatar::scaled(n, scale_); };
    const bool blink = elapsed_ms % avatar_config::CONVERSATION_BLINK_PERIOD_MS >=
        avatar_config::CONVERSATION_BLINK_PERIOD_MS - avatar_config::CONVERSATION_BLINK_MS;
    // 聞き終わりのうなずきが済んでから、待ち時間に合わせて目だけを斜め上へ動かす。
    const int look = static_cast<int>(std::clamp<uint32_t>(elapsed_ms, 650, 950) - 650);
    for (int side = 0; side < 2; ++side) {
        // 左の薄目は右の丸い目の半分の高さ。瞬き中だけは両目を同じ細さへ閉じる。
        lv_obj_set_size(eyes_[side], px(30), px(blink ? 2 : side ? 30 : 15));
        lv_obj_align(eyes_[side], LV_ALIGN_CENTER, px((side ? 70 : -70) + 12 * look / 300),
                     px((side ? -18 : -15) - 10 * look / 300));
    }
    // 手動プレビュー中でも、実際に音声が出ている場合だけ振幅で口を開く。
    const int opening = speaking ? std::min<unsigned>(mouth, 100) : 0;
    lv_obj_set_size(mouth_, px(22), px(7 + opening * 18 / 100));
    lv_obj_set_style_radius(mouth_, px(4), 0);
    lv_obj_align(mouth_, LV_ALIGN_CENTER, px(8), px(37));
    const unsigned active = elapsed_ms / avatar_config::THINKING_DOT_INTERVAL_MS % 3;
    for (unsigned i = 0; i < 3; ++i)
        lv_obj_set_style_bg_color(dots_[i], i == active ? lv_color_hex(0x8bdcff) : lv_color_white(), 0);
}
