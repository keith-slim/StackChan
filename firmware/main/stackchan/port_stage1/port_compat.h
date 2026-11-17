#pragma once
#include <lvgl.h>
// S3-stackchan's 9.6 convenience functions, expressed using LVGL 9.5's
// stable flag API. Kept local to copied effect implementations.
inline void stage1_set_hidden(lv_obj_t* obj, bool hidden) {
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}
inline void stage1_set_clickable(lv_obj_t* obj, bool enabled) {
    if (enabled) lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}
inline void stage1_set_scrollable(lv_obj_t* obj, bool enabled) {
    if (enabled) lv_obj_add_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}
#define lv_obj_set_hidden stage1_set_hidden
#define lv_obj_set_clickable stage1_set_clickable
#define lv_obj_set_scrollable stage1_set_scrollable
