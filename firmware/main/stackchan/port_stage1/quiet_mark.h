#pragma once
#include <lvgl.h>

// Two crossed white tape strips over the mouth, adapted from
// S3-stackchan/gemini-live. No bitmap, extra buffer or timer.
// Owned only by the active Stage1 avatar screen under the LVGL lock.
class AvatarQuietMark {
public:
    explicit AvatarQuietMark(lv_obj_t* parent);
    ~AvatarQuietMark();
    void Render(lv_obj_t* mouth);
private:
    static void Draw(lv_event_t* event);
    lv_obj_t* root_ = nullptr;
    lv_obj_t* anchor_ = nullptr;
    int scale_ = 256;
};
