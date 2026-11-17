#include "port_compat.h"
#include "rest_face.h"
#include "avatar_viewport.h"
#include "yawn_animation.h"
#include <algorithm>
#include <cmath>

LV_IMAGE_DECLARE(decorator_sweat);

AvatarRestFace::AvatarRestFace(lv_obj_t *parent)
{
    root_ = lv_obj_create(parent);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, lv_pct(100), lv_pct(100));
    lv_obj_set_clickable(root_, false);
    lv_obj_set_scrollable(root_, false);
    scale_ = stackchan::avatar::viewport_scale(parent);
    for (auto &eye : eyes_) {
        eye = lv_line_create(root_);
        lv_obj_set_style_line_color(eye, lv_color_white(), 0);
        lv_obj_set_style_line_width(eye, std::max(1, stackchan::avatar::scaled(4, scale_)), 0);
        lv_obj_set_style_line_rounded(eye, true, 0);
    }
    mouth_ = lv_obj_create(root_);
    drop_ = lv_obj_create(root_);
    for (auto *object : {mouth_, drop_}) {
        lv_obj_remove_style_all(object);
        lv_obj_set_clickable(object, false);
        lv_obj_set_scrollable(object, false);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    }
    lv_obj_set_style_bg_color(mouth_, lv_color_white(), 0);
    lv_obj_set_style_bg_color(drop_, lv_color_hex(0x8bdcff), 0);
    lv_obj_set_style_radius(drop_, LV_RADIUS_CIRCLE, 0);
    drool_ = lv_line_create(root_);
    lv_obj_set_style_line_color(drool_, lv_color_hex(0x8bdcff), 0);
    lv_obj_set_style_line_width(drool_, std::max(1, stackchan::avatar::scaled(2, scale_)), 0);
    lv_obj_set_style_line_rounded(drool_, true, 0);
    sweat_ = lv_image_create(root_);
    lv_image_set_src(sweat_, &decorator_sweat);
    lv_image_set_scale(sweat_, scale_ * 3 / 5);
    lv_obj_set_style_image_recolor(sweat_, lv_color_hex(0x8bdcff), 0);
    lv_obj_set_style_image_recolor_opa(sweat_, LV_OPA_COVER, 0);
}

AvatarRestFace::~AvatarRestFace() { lv_obj_delete(root_); }

void AvatarRestFace::Render(AvatarRestPose pose, bool speaking, uint8_t mouth, uint32_t elapsed_ms, bool hide_sleep_bubble)
{
    // 起床演出の黒背景は通常の顔へ持ち越さず、鼻提灯も寝顔だけに限定する。
    lv_obj_set_style_bg_opa(root_, LV_OPA_TRANSP, 0);
    bubble_popping_ = false;
    if (pose != AvatarRestPose::Sleeping && bubble_) {
        lv_obj_delete(bubble_);
        bubble_ = nullptr;
    }
    if(pose==AvatarRestPose::Yawning){pose_=pose;RenderYawn(elapsed_ms);return;}
    // 黒い口の内側と舌を通常の寝顔へ残さず、専用部品のメモリも返す。
    if(mouth_inside_){lv_obj_delete(mouth_inside_);mouth_inside_=nullptr;}
    if(tongue_){lv_obj_delete(tongue_);tongue_=nullptr;}
    const auto px = [this](int n) { return stackchan::avatar::scaled(n, scale_); };
    const int cx = lv_obj_get_width(root_) / 2, cy = lv_obj_get_height(root_) / 2;
    if (pose_ != pose) {
        pose_ = pose;
        for (int side = 0; side < 2; ++side) {
            lv_obj_set_style_line_width(eyes_[side], std::max(1,px(4)),0);
            if (pose == AvatarRestPose::Sleeping) {
                // 下向きの弧にして、喜びの目と寝顔を区別する。
                for (int i = 0; i < 9; ++i) {
                    const float x = (i - 4) / 4.0f;
                    eye_points_[side][i] = {static_cast<lv_value_precise_t>(px(i * 3)), static_cast<lv_value_precise_t>(px(static_cast<int>(4 * (1 - x * x))))};
                }
                lv_line_set_points(eyes_[side], eye_points_[side], 9);
                lv_obj_set_pos(eyes_[side], cx + px((side ? 70 : -70) - 12), cy + px(-17));
            } else {
                // 起こされた瞬間は「＞ ＜」。朝の定時起床ではこの姿勢を指定しない。
                for (int i = 0; i < 3; ++i) eye_points_[side][i] = {
                    static_cast<lv_value_precise_t>(px((i == 1) == (side == 0) ? 16 : 0)),
                    static_cast<lv_value_precise_t>(px(i * 9))};
                lv_line_set_points(eyes_[side], eye_points_[side], 3);
                lv_obj_set_pos(eyes_[side], cx + px((side ? 70 : -70) - 8), cy + px(-25));
            }
        }
        lv_obj_set_hidden(drool_, pose != AvatarRestPose::Sleeping);
        lv_obj_set_hidden(drop_, pose != AvatarRestPose::Sleeping);
        lv_obj_set_hidden(sweat_, pose != AvatarRestPose::Startled);
        lv_obj_align(sweat_, LV_ALIGN_CENTER, px(89), px(-43));
    }
    // 寝息は描画専用。マイク・WakeNetの発話停止フラグには触れない。
    const float breath = (1.0f - std::cos((elapsed_ms % 4000) * 6.283185307f / 4000)) * 0.5f;
    const float opening = speaking ? std::min<unsigned>(mouth, 100) / 100.0f : breath;
    const bool sleep = pose == AvatarRestPose::Sleeping;
    const int w = px(sleep ? static_cast<int>(50 - 12 * opening) : speaking ? static_cast<int>(32 - 10 * opening) : 22);
    const int h = px(sleep ? static_cast<int>(6 + 17 * opening) : speaking ? static_cast<int>(4 + 28 * opening) : 28);
    lv_obj_set_size(mouth_, w, h);
    lv_obj_set_style_radius(mouth_, sleep ? px(static_cast<int>(3 + 7 * opening)) : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(mouth_, cx - w / 2, cy + px(26) - h / 2);
    if (sleep) {
        // 寝言の発話中は既存部品を隠すだけにし、終了時は呼吸位置へ戻して再表示する。
        RenderBubble(opening);
        if(bubble_)lv_obj_set_hidden(bubble_,hide_sleep_bubble);
        const int x = cx + w / 2 - px(4), y = cy + px(26) + h / 2 - px(1);
        drool_points_[0] = {0, 0}; drool_points_[1] = {static_cast<lv_value_precise_t>(px(1)), static_cast<lv_value_precise_t>(px(5))};
        drool_points_[2] = {0, static_cast<lv_value_precise_t>(px(10))}; drool_points_[3] = {static_cast<lv_value_precise_t>(px(1)), static_cast<lv_value_precise_t>(px(14))};
        lv_line_set_points(drool_, drool_points_, 4);
        lv_obj_set_pos(drool_, x, y);
        lv_obj_set_size(drop_, std::max(1, px(5)), std::max(1, px(6)));
        lv_obj_set_pos(drop_, x - px(1), y + px(12));
    }
}

void AvatarRestFace::RenderWakePop(uint32_t sleep_elapsed_ms, uint32_t pop_elapsed_ms)
{
    Render(AvatarRestPose::Sleeping, false, 0, sleep_elapsed_ms);
    // 論理上の起床・発話は進めたまま、背後の開いた目だけを240ms覆う。
    lv_obj_set_style_bg_color(root_, lv_color_hex(avatar_config::BACKGROUND_COLOR), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_move_foreground(root_);
    bubble_popping_ = true;
    bubble_pop_elapsed_ = pop_elapsed_ms;
    lv_obj_invalidate(bubble_);
}

void AvatarRestFace::RenderBubble(float opening)
{
    if (!bubble_) {
        bubble_ = lv_obj_create(root_);
        lv_obj_remove_style_all(bubble_);
        lv_obj_set_clickable(bubble_, false);
        lv_obj_set_scrollable(bubble_, false);
        lv_obj_add_event_cb(bubble_, DrawBubble, LV_EVENT_DRAW_MAIN, this);
        lv_obj_set_size(bubble_, stackchan::avatar::scaled(70, scale_), stackchan::avatar::scaled(72, scale_));
    }
    // 呼吸といびきの口パクの両方で、口が閉じた時に最大、開いた時に最小にする。
    bubble_radius_ = 4 + 14 * (1 - opening);
    const int cx = lv_obj_get_width(root_) / 2, cy = lv_obj_get_height(root_) / 2;
    lv_obj_set_pos(bubble_, cx + stackchan::avatar::scaled(4, scale_), cy - stackchan::avatar::scaled(54, scale_));
    lv_obj_invalidate(bubble_);
}

void AvatarRestFace::DrawBubble(lv_event_t *event)
{
    auto *self = static_cast<AvatarRestFace *>(lv_event_get_user_data(event));
    auto *layer = lv_event_get_layer(event);
    lv_area_t bounds;
    lv_obj_get_coords(self->bubble_, &bounds);
    const auto point = [&](float x, float y) {
        const float scale = self->scale_ / static_cast<float>(avatar_config::SCALE_UNIT);
        return lv_point_precise_t{static_cast<lv_value_precise_t>(bounds.x1 + std::lround(x * scale)),
                                  static_cast<lv_value_precise_t>(bounds.y1 + std::lround(y * scale))};
    };
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.width = std::max(1, stackchan::avatar::scaled(1, self->scale_));
    line.color = lv_color_hex(0xa9eaff);
    line.round_start = line.round_end = 1;
    const auto stroke = [&](lv_point_precise_t from, lv_point_precise_t to) {
        line.p1 = from;
        line.p2 = to;
        lv_draw_line(layer, &line);
    };
    // 輪郭を一瞬消して放射線を広げ、最後は薄く消してから目を開く。
    constexpr uint32_t snap_ms = 60;
    if (self->bubble_popping_ && self->bubble_pop_elapsed_ >= snap_ms) {
        const float t = std::min(1.0f, (self->bubble_pop_elapsed_ - snap_ms) /
                                     static_cast<float>(avatar_config::SLEEP_BUBBLE_POP_MS - snap_ms));
        line.opa = static_cast<lv_opa_t>(static_cast<float>(LV_OPA_COVER) * (1 - t));
        line.color = lv_color_white();
        for (int i = 0; i < 8; ++i) {
            const float a = i * 6.283185307f / 8;
            const float dx = std::cos(a), dy = std::sin(a);
            stroke(point(28 + dx * (10 + 10 * t), 31 + dy * (10 + 10 * t)),
                   point(28 + dx * (18 + 7 * t), 31 + dy * (18 + 7 * t)));
        }
        return;
    }
    const float radius = self->bubble_popping_ ? 18 + 2 * self->bubble_pop_elapsed_ / snap_ms : self->bubble_radius_;
    // 鼻先を固定した丸いしずく形。画像や全画面の中間バッファは確保しない。
    constexpr float curves[][8] = {
        {0, 0, .7f, -.55f, .05f, -.5f, .1f, -1.25f},
        {.1f, -1.25f, .1f, -1.9f, .55f, -2.4f, 1.2f, -2.35f},
        {1.2f, -2.35f, 1.9f, -2.3f, 2.4f, -1.85f, 2.3f, -1.15f},
        {2.3f, -1.15f, 2.2f, -.25f, 1.15f, -.2f, 0, 0}
    };
    lv_point_precise_t outline[49];
    unsigned n = 0;
    for (const auto &c : curves) for (int i = 0; i < 12; ++i) {
        const float t = i / 12.0f, u = 1 - t;
        const float x = u*u*u*c[0] + 3*u*u*t*c[2] + 3*u*t*t*c[4] + t*t*t*c[6];
        const float y = u*u*u*c[1] + 3*u*u*t*c[3] + 3*u*t*t*c[5] + t*t*t*c[7];
        outline[n++] = point(9 + radius * x, 54 + radius * y);
    }
    outline[n] = outline[0];
    for (unsigned i = 0; i < n; ++i) stroke(outline[i], outline[i + 1]);
    // 白い短い光沢で、よだれとは違う薄い膜に見せる。
    line.color = lv_color_white();
    stroke(point(9 + .45f * radius, 54 - 1.35f * radius), point(9 + .5f * radius, 54 - 1.7f * radius));
    stroke(point(9 + .5f * radius, 54 - 1.7f * radius), point(9 + .75f * radius, 54 - 1.9f * radius));
}

void AvatarRestFace::RenderYawn(uint32_t elapsed_ms)
{
    // 640×480の採用図を現在の表示倍率へ直接写し、全画面の中間画像は作らない。
    const auto unit=[this](float value){return static_cast<int>(std::lround(value*scale_/(2.0f*avatar_config::SCALE_UNIT)));};
    const int cx=lv_obj_get_width(root_)/2,cy=lv_obj_get_height(root_)/2;
    const auto frame=avatar_yawn::At(elapsed_ms);
    const float open=frame.opening;
    for(int side=0;side<2;++side){
        if(frame.drowsy){
            const float thickness=14-11*open,length=40-thickness;
            eye_points_[side][0]={0,0};eye_points_[side][1]={static_cast<lv_value_precise_t>(unit(length)),0};
            lv_line_set_points(eyes_[side],eye_points_[side],2);
            lv_obj_set_style_line_width(eyes_[side],std::max(1,unit(thickness)),0);
            lv_obj_set_pos(eyes_[side],cx+unit((side?140:-140)-length/2),cy+unit(-32));
        }else{
            const float half=24+4*open,depth=18+6*open;
            for(int i=0;i<9;++i){const float t=i/8.0f;
                eye_points_[side][i]={static_cast<lv_value_precise_t>(unit(2*half*t)),static_cast<lv_value_precise_t>(unit(2*(1-t)*t*depth))};
            }
            lv_line_set_points(eyes_[side],eye_points_[side],9);
            lv_obj_set_style_line_width(eyes_[side],std::max(1,unit(8)),0);
            lv_obj_set_pos(eyes_[side],cx+unit((side?140:-140)-half),cy+unit(-34-4*open));
        }
    }
    // 追加部品は必要になった時だけ作る。音声タスク・マイク待受には触れない。
    if(!mouth_inside_){
        mouth_inside_=lv_obj_create(root_);tongue_=lv_obj_create(root_);
        for(auto *object:{mouth_inside_,tongue_}){
            lv_obj_remove_style_all(object);lv_obj_set_clickable(object,false);lv_obj_set_scrollable(object,false);
            lv_obj_set_style_bg_opa(object,LV_OPA_COVER,0);
        }
        lv_obj_set_style_bg_color(mouth_inside_,lv_color_black(),0);
        lv_obj_set_style_bg_color(tongue_,lv_color_white(),0);
        lv_obj_set_style_radius(tongue_,LV_RADIUS_CIRCLE,0);
    }
    const float a=frame.drowsy?0:open,w=100-26*a,h=12+100*a,mouth_y=52+8*a,radius=std::min(w,h)/2;
    lv_obj_set_size(mouth_,unit(w),unit(h));lv_obj_set_style_radius(mouth_,unit(radius),0);
    lv_obj_set_pos(mouth_,cx-unit(w)/2,cy+unit(mouth_y)-unit(h)/2);
    const float iw=w-16,ih=std::max(0.0f,h-16);
    lv_obj_set_size(mouth_inside_,unit(iw),unit(ih));lv_obj_set_style_radius(mouth_inside_,unit(std::min(iw,ih)/2),0);
    lv_obj_set_pos(mouth_inside_,cx-unit(iw)/2,cy+unit(mouth_y)-unit(ih)/2);
    lv_obj_set_hidden(mouth_inside_,frame.drowsy||ih<=0);
    const int tongue_width=unit(2*std::min(16.0f,iw/3));
    lv_obj_set_size(tongue_,tongue_width,std::max(1,unit(12)));
    lv_obj_set_pos(tongue_,cx-tongue_width/2,cy+unit(mouth_y+h/2-23)-unit(6));
    lv_obj_set_hidden(tongue_,frame.drowsy||h<=60);
    const float round=radius*(1-0.707106781f);
    const int x=cx+unit(w/2-8+(8-round)*a),y=cy+unit(mouth_y+h/2-1+(1-round)*a);
    for(int i=0;i<9;++i){const float t=i/8.0f,u=1-t;
        drool_points_[i]={static_cast<lv_value_precise_t>(unit(3*u*u*t*5+3*u*t*t*3+t*t*t*3)),
                         static_cast<lv_value_precise_t>(unit(3*u*u*t*8+3*u*t*t*17+t*t*t*25))};
    }
    lv_line_set_points(drool_,drool_points_,9);lv_obj_set_pos(drool_,x,y);
    lv_obj_set_size(drop_,std::max(1,unit(9)),std::max(1,unit(12)));
    lv_obj_set_pos(drop_,x+unit(3)-unit(4.5f),y+unit(23));
    lv_obj_set_hidden(drool_,frame.drowsy);lv_obj_set_hidden(drop_,frame.drowsy);lv_obj_set_hidden(sweat_,true);
}
