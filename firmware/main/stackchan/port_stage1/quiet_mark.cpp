#include "quiet_mark.h"
#include "avatar_viewport.h"
#include <algorithm>

// S3-stackchan/gemini-live/src/avatar/quiet_mark.cpp, using the
// existing LVGL 9 draw callback style. Objects are owned by Stage1.
AvatarQuietMark::AvatarQuietMark(lv_obj_t* parent) {
    scale_ = stackchan::avatar::viewport_scale(parent) * 3 / 2;
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, stackchan::avatar::scaled(34, scale_) + 1,
                           stackchan::avatar::scaled(30, scale_) + 1);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root_, Draw, LV_EVENT_DRAW_MAIN, this);
}

AvatarQuietMark::~AvatarQuietMark() {
    if (root_) lv_obj_delete(root_);
}

void AvatarQuietMark::Render(lv_obj_t* mouth) {
    if (!root_ || !mouth) return;
    // An autonomous rest face may have appeared after this overlay.
    // Only change z-order when its mouth reference changes.
    if (anchor_ != mouth) {
        anchor_ = mouth;
        lv_obj_move_foreground(root_);
    }
    lv_obj_align_to(root_, mouth, LV_ALIGN_CENTER, 0, 0);
}

void AvatarQuietMark::Draw(lv_event_t* event) {
    auto* self = static_cast<AvatarQuietMark*>(lv_event_get_user_data(event));
    auto* layer = lv_event_get_layer(event);
    lv_area_t bounds{};
    lv_obj_get_coords(self->root_, &bounds);
    const int center_x = (bounds.x1 + bounds.x2) / 2;
    const int center_y = (bounds.y1 + bounds.y2) / 2;
    const auto px = [self](int value) {
        return stackchan::avatar::scaled(value, self->scale_);
    };
    const auto strip = [&](int rise, uint32_t fill) {
        lv_draw_line_dsc_t line;
        lv_draw_line_dsc_init(&line);
        line.p1 = {static_cast<lv_value_precise_t>(center_x - px(10)),
                   static_cast<lv_value_precise_t>(center_y - px(rise))};
        line.p2 = {static_cast<lv_value_precise_t>(center_x + px(10)),
                   static_cast<lv_value_precise_t>(center_y + px(rise))};
        line.round_start = line.round_end = 1;
        line.width = std::max(1, px(14));
        line.color = lv_color_hex(0x6e6e6e);
        lv_draw_line(layer, &line);
        line.width = std::max(1, px(11));
        line.color = lv_color_hex(fill);
        lv_draw_line(layer, &line);
    };
    strip(6, 0xdedede);
    strip(-8, 0xf7f7f7);
}
