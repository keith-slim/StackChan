#pragma once
#include <lvgl.h>
#include <stackchan/config/low_battery.h>
#include <cstdint>
#include <cstdio>

namespace view {
// Shared by the pull-down status bar and the persistent avatar indicator.
// No I2C polling, image buffer or timer: consume the existing power sample.
class BatteryIndicator {
public:
    BatteryIndicator(lv_obj_t* parent, uint32_t normal, bool backdrop = false)
        : normal_(normal) {
        root_ = lv_obj_create(parent);
        lv_obj_remove_style_all(root_);
        lv_obj_set_size(root_, 84, 22);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
        if (backdrop) {
            lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(root_, LV_OPA_60, 0);
            lv_obj_set_style_radius(root_, 4, 0);
        }
        label_ = lv_label_create(root_);
        lv_obj_set_style_text_font(label_, &lv_font_montserrat_16, 0);
        lv_obj_align(label_, LV_ALIGN_RIGHT_MID, -38, 0);
        outline_ = Part(30, 12, 47, 5);
        lv_obj_set_style_radius(outline_, 3, 0);
        lv_obj_set_style_border_width(outline_, 1, 0);
        lv_obj_set_style_bg_opa(outline_, LV_OPA_TRANSP, 0);
        terminal_ = Part(3, 5, 78, 9);
        fill_ = Part(26, 8, 49, 7);
        lv_obj_set_style_radius(fill_, 1, 0);
        // Tiny line-based lightning mark avoids initializing the assets
        // partition early just to show the battery during startup.
        static const lv_point_precise_t lightning[] = {{5,0},{0,5},{5,5},{2,11},{9,4},{4,4},{5,0}};
        bolt_ = lv_line_create(root_);
        lv_line_set_points(bolt_, lightning, 7);
        lv_obj_set_pos(bolt_, 57, 5);
        lv_obj_set_style_line_color(bolt_, lv_color_white(), 0);
        lv_obj_set_style_line_width(bolt_, 1, 0);
        lv_obj_remove_flag(bolt_, LV_OBJ_FLAG_CLICKABLE);
        Update();
    }
    ~BatteryIndicator() { lv_obj_delete(root_); }
    BatteryIndicator(const BatteryIndicator&) = delete;
    BatteryIndicator& operator=(const BatteryIndicator&) = delete;
    void Align(lv_align_t alignment, int x, int y) { lv_obj_align(root_, alignment, x, y); }
    void Update() {
        using namespace stackchan::low_battery;
        const int bits = sample_bits.load();
        if (bits == last_) return;
        last_ = bits;
        const int level = bits < 0 ? -1 : bits & 127;
        const bool charging = bits >= 0 && (bits & 128);
        uint32_t rgb = normal_;
        if (level >= 0 && level <= kShutdownLevel) rgb = 0xff4343;
        else if (level >= 0 && level <= kWarningLevel) rgb = 0xeab308;
        else if (charging) rgb = 0x19c25f;
        const auto color = lv_color_hex(rgb);
        char text[8];
        if (level < 0) std::snprintf(text,sizeof(text),"--%%");
        else std::snprintf(text,sizeof(text),"%d%%",level);
        lv_label_set_text(label_, text);
        lv_obj_set_style_text_color(label_,color,0);
        lv_obj_set_style_border_color(outline_,color,0);
        lv_obj_set_style_bg_color(terminal_,color,0);
        lv_obj_set_style_bg_color(fill_,color,0);
        if (level > 0) {
            lv_obj_set_width(fill_, (26 * level + 99) / 100);
            lv_obj_remove_flag(fill_, LV_OBJ_FLAG_HIDDEN);
        } else lv_obj_add_flag(fill_, LV_OBJ_FLAG_HIDDEN);
        if (charging) lv_obj_remove_flag(bolt_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(bolt_, LV_OBJ_FLAG_HIDDEN);
    }
private:
    lv_obj_t* Part(int w, int h, int x, int y) {
        auto* obj = lv_obj_create(root_);
        lv_obj_remove_style_all(obj);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(obj,w,h);
        lv_obj_set_pos(obj,x,y);
        lv_obj_set_style_bg_opa(obj,LV_OPA_COVER,0);
        return obj;
    }
    lv_obj_t *root_ = nullptr, *label_ = nullptr, *outline_ = nullptr,
             *terminal_ = nullptr, *fill_ = nullptr, *bolt_ = nullptr;
    uint32_t normal_;
    int last_ = -2;
};
} // namespace view
