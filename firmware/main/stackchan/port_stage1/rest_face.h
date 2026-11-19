#pragma once
#include "avatar_expression.h"
#include "lvgl.h"

// 寝顔を使う間だけ小さなLVGL部品を保持し、通常時の通信メモリを圧迫しない。
class AvatarRestFace {
public:
    explicit AvatarRestFace(lv_obj_t *parent);
    ~AvatarRestFace();
    void Render(AvatarRestPose pose, bool speaking, uint8_t mouth, uint32_t elapsed_ms=0, bool hide_sleep_decorations=false);
    // 寝顔を短く残して鼻提灯だけをはじけさせ、次の顔へつなぐ。
    void RenderWakePop(uint32_t sleep_elapsed_ms, uint32_t pop_elapsed_ms);
    // 寝顔・あくびの口に装飾を合わせるための参照。所有権は渡さない。
    lv_obj_t *MouthObject() const { return mouth_; }
private:
    void RenderYawn(uint32_t elapsed_ms);
    void RenderBubble(float opening); // 口が閉じるほど鼻提灯を大きくし、同じ呼吸周期へ合わせる。
    static void DrawBubble(lv_event_t *event); // 小さな部品内で描き、部分更新の範囲外へ残像を残さない。
    lv_obj_t *root_, *eyes_[2], *mouth_, *drool_, *drop_, *sweat_;
    lv_obj_t *mouth_inside_=nullptr, *tongue_=nullptr; // あくび中だけ確保し、寝顔へ移ると解放する。
    lv_obj_t *bubble_=nullptr; // 鼻提灯は寝顔の間だけ確保し、他の顔では部品ごと解放する。
    float bubble_radius_=0;
    bool bubble_popping_=false;
    uint32_t bubble_pop_elapsed_=0;
    lv_point_precise_t eye_points_[2][9] = {}, drool_points_[9] = {};
    AvatarRestPose pose_ = AvatarRestPose::None;
    int scale_;
};
