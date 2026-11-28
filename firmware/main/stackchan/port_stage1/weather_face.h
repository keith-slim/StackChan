#pragma once
#include "lvgl.h"
#include "weather_conditions.h"

namespace stackchan::stage1 {

// 天気の小さな飾りと気温の装飾だけを描く。全画面画像・専用タイマーは持たない。
class AvatarWeatherFace {
public:
    explicit AvatarWeatherFace(lv_obj_t *parent);
    ~AvatarWeatherFace();
    void Render(const weather::Appearance &appearance,uint32_t elapsed_ms);
private:
    static void Draw(lv_event_t *event);
    lv_obj_t *badge_,*comfort_;
    weather::Appearance appearance_;
    int scale_;
    uint32_t key_=UINT32_MAX,phase_=0;
};

} // namespace stackchan::stage1
