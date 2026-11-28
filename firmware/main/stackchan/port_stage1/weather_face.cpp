#include "weather_face.h"
#include "port_compat.h"
#include "avatar_viewport.h"
#include <algorithm>

namespace stackchan::stage1 {

namespace {
// 既存の部分描画に任せ、線端も各部品の更新範囲に収める。
class Painter {
public:
    Painter(lv_event_t *event,int scale):layer(lv_event_get_layer(event)),scale_(scale){lv_obj_get_coords(lv_event_get_target_obj(event),&area);}
    lv_point_precise_t point(int x,int y)const{return {static_cast<lv_value_precise_t>(area.x1+px(x)),static_cast<lv_value_precise_t>(area.y1+px(y))};}
    int px(int n)const{return stackchan::avatar::scaled(n,scale_);}
    void box(int x,int y,int w,int h,int radius,uint32_t color){
        lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(color);d.bg_opa=LV_OPA_COVER;d.radius=px(radius);
        const auto p=point(x,y);lv_area_t r{static_cast<int32_t>(p.x),static_cast<int32_t>(p.y),static_cast<int32_t>(p.x)+px(w)-1,static_cast<int32_t>(p.y)+px(h)-1};lv_draw_rect(layer,&d,&r);
    }
    void line(int x,int y,int x2,int y2,int width,uint32_t color){
        lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.p1=point(x,y);d.p2=point(x2,y2);d.width=std::max(1,px(width));d.color=lv_color_hex(color);d.round_start=d.round_end=1;lv_draw_line(layer,&d);
    }
    void triangle(int x,int y,int x2,int y2,int x3,int y3,uint32_t color){
        lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);d.p[0]=point(x,y);d.p[1]=point(x2,y2);d.p[2]=point(x3,y3);d.color=lv_color_hex(color);lv_draw_triangle(layer,&d);
    }
    void sun(bool night,int x=34,int y=20){
        if(night){box(x-11,y-11,22,22,11,0xe4edff);box(x-3,y-15,22,22,11,0x000000);}
        else{
            for(const auto p:{lv_point_t{0,-16},lv_point_t{11,-11},lv_point_t{16,0},lv_point_t{11,11},lv_point_t{0,16},lv_point_t{-11,11},lv_point_t{-16,0},lv_point_t{-11,-11}})
                line(x+p.x*3/4,y+p.y*3/4,x+p.x,y+p.y,2,0xffd36b);
            box(x-9,y-9,18,18,9,0xffc75c);
        }
    }
    void cloud(int x,int y,uint32_t color=0xd8e5f0){
        box(x-20,y-3,40,14,7,color);box(x-15,y-9,17,17,9,color);box(x-5,y-14,23,23,12,color);box(x+9,y-7,17,17,9,color);
    }
    void flake(int x,int y,int radius,uint32_t color=0xe8f8ff){line(x-radius,y,x+radius,y,1,color);line(x-radius/2,y-radius,x+radius/2,y+radius,1,color);line(x-radius/2,y+radius,x+radius/2,y-radius,1,color);}
    lv_layer_t *layer;
private:lv_area_t area{};int scale_;
};
bool animated(weather::Kind kind)
{
    using K=weather::Kind;
    return kind!=K::Clear&&kind!=K::Clouds&&kind!=K::Unknown;
}
}

AvatarWeatherFace::AvatarWeatherFace(lv_obj_t *parent):scale_(stackchan::avatar::viewport_scale(parent))
{
    const auto make=[&]{auto *o=lv_obj_create(parent);lv_obj_remove_style_all(o);lv_obj_set_clickable(o,false);lv_obj_set_scrollable(o,false);lv_obj_add_event_cb(o,Draw,LV_EVENT_DRAW_MAIN,this);return o;};
    badge_=make();comfort_=make();
    lv_obj_set_size(badge_,stackchan::avatar::scaled(68,scale_),stackchan::avatar::scaled(56,scale_));
    lv_obj_align(badge_,LV_ALIGN_TOP_LEFT,stackchan::avatar::scaled(7,scale_),stackchan::avatar::scaled(6,scale_));
}
AvatarWeatherFace::~AvatarWeatherFace(){lv_obj_delete(comfort_);lv_obj_delete(badge_);}

void AvatarWeatherFace::Render(const weather::Appearance &appearance,uint32_t elapsed_ms)
{
    // 起床時の黒背景や後から追加された顔より前に保つ。順番が同じなら移動も再描画も増やさない。
    if(lv_obj_get_child(lv_obj_get_parent(badge_),-1)!=comfort_){
        lv_obj_move_foreground(badge_);lv_obj_move_foreground(comfort_);
    }
    const auto &c=appearance.condition;
    const uint32_t key=static_cast<uint32_t>(c.kind)|(c.strength<<5)|(c.clouds<<8)|(c.lightning<<11)|
        (c.showers?1U<<13:0)|(static_cast<uint32_t>(appearance.comfort)<<14)|(appearance.night?1U<<16:0)|(appearance.visible?1U<<17:0);
    const auto phase=animated(c.kind)?elapsed_ms/240U%16U:0;
    if(key_==key&&phase_==phase)return;
    const bool changed=key_!=key;key_=key;phase_=phase;appearance_=appearance;
    lv_obj_set_hidden(badge_,!appearance.visible);lv_obj_set_hidden(comfort_,!appearance.visible||appearance.comfort==weather::Comfort::None);
    if(changed){
        const bool cold=appearance.comfort==weather::Comfort::Cold;
        lv_obj_set_size(comfort_,stackchan::avatar::scaled(cold?108:28,scale_),stackchan::avatar::scaled(cold?34:42,scale_));
        lv_obj_align(comfort_,LV_ALIGN_CENTER,stackchan::avatar::scaled(cold?0:107,scale_),stackchan::avatar::scaled(cold?76:-58,scale_));
        lv_obj_invalidate(comfort_);
    }
    lv_obj_invalidate(badge_); // 雨粒などは小さな部品だけを更新し、顔全体を再描画しない。
}

void AvatarWeatherFace::Draw(lv_event_t *event)
{
    auto *self=static_cast<AvatarWeatherFace *>(lv_event_get_user_data(event));
    if(!self->appearance_.visible)return;
    Painter p(event,self->scale_);
    if(lv_event_get_target_obj(event)==self->comfort_){
        if(self->appearance_.comfort==weather::Comfort::Cold){
            p.box(3,4,102,15,6,0xe88773);p.box(66,15,15,16,3,0xd97061);
            for(int x:{16,37,58,85})p.box(x,5,4,13,1,0xffdbb6);
            p.line(69,28,69,31,1,0xffdbb6);p.line(74,28,74,31,1,0xffdbb6);p.line(79,28,79,31,1,0xffdbb6);
        }else if(self->appearance_.comfort==weather::Comfort::Hot){
            for(int i=0;i<3;++i){const int x=4+i*8,y=4+i*7;p.triangle(x+3,y,x,y+8,x+6,y+8,0x8bddff);p.box(x,y+5,7,9,4,0x8bddff);}
        }
        return;
    }
    using K=weather::Kind;const auto &a=self->appearance_;const auto &c=a.condition;const int phase=self->phase_;
    if(c.kind==K::Clear){p.sun(a.night,34,25);return;}
    if(c.kind==K::Clouds){
        if(c.clouds<=2)p.sun(a.night,23,18);
        if(c.clouds>=3)p.cloud(29,17,0x8798b0);
        p.cloud(36,c.clouds>=3?32:30,c.clouds==4?0xb0bdce:0xe4edf5);
        if(c.clouds==2)p.cloud(27,35,0xc9d9e8);
        return;
    }
    const bool rain=c.kind==K::Rain||c.kind==K::FreezingRain;
    const bool snow=c.kind==K::Snow||c.kind==K::Sleet;
    if(rain){
        if(c.showers)p.sun(a.night,18,18); // にわか雨は太陽・月を傘の後ろへ添える。
        // 小雨から猛烈な雨まで、傘の色・雨粒の数・太さ・流れる速さを段階的に変える。
        const int sway=c.strength>=4?(phase%4<2?-1:1):0;
        p.box(12+sway,3,44,44,22,c.strength>=4?0x428ac7:0x8bd6f4);p.box(10,25,49,24,0,0x000000);
        for(int x:{19,33,47})p.box(x+sway,22,14,9,7,0x000000);
        p.line(34+sway,23,34+sway,39,2,0xe7eff7);p.line(34+sway,39,39+sway,42,2,0xe7eff7);p.line(39+sway,42,43+sway,39,2,0xe7eff7);
        if(c.kind==K::FreezingRain)for(int x:{21,34,47})p.flake(x,16,3);
    }else if(c.kind==K::Drizzle||c.kind==K::Thunderstorm||snow){
        if(c.showers)p.sun(a.night,17,13);
        p.cloud(32,19,c.kind==K::Thunderstorm?0x98a8c1:0xd5e4f1);
    }
    if(rain||snow||c.kind==K::Drizzle||c.kind==K::Thunderstorm){
        const int count=c.strength?2+c.strength*2:0;
        for(int i=0;i<count;++i){
            const int x=7+(i*17)%54,y=35+(phase*(c.strength>=4?3:1)+i*5)%15;
            if(snow&&(c.kind!=K::Sleet||i%2==0))p.flake(x,y,2+(c.strength>=4));
            else if(c.kind==K::Drizzle)p.line(x,y,x,y+2,1,0xb3d9ed);
            else p.line(x,y,x-(c.strength>=3?3:1),std::min(53,y+2+c.strength),c.strength>=4?2:1,0x83cdff);
        }
        for(int i=0;i<c.lightning;++i){const int x=13+i*17;p.line(x+6,27,x,35,2,0xffd765);p.line(x,35,x+5,35,2,0xffd765);p.line(x+5,35,x-1,44,2,0xffd765);}
        return;
    }
    if(c.kind==K::Mist||c.kind==K::Fog||c.kind==K::Haze){
        if(c.kind!=K::Fog)p.sun(a.night,32,18);
        const int count=c.kind==K::Fog?5:3;
        for(int i=0;i<count;++i){const int shift=(phase+i*3)%5;p.line(8+shift,22+i*6,57-shift,22+i*6,2,c.kind==K::Haze?0xd6c59b:0xc2d1df);}
    }else if(c.kind==K::Smoke){
        for(int i=0;i<3;++i){const int x=16+i*17;p.line(x,46,x+3,34,3,0x8392a4);p.line(x+3,34,x-2,22,3,0xb0bcc9);p.box(x-7+(phase+i)%3,9+i*3,16,15,8,0xc2cbd6);}
    }else if(c.kind==K::Dust||c.kind==K::Sand||c.kind==K::Ash){
        const uint32_t color=c.kind==K::Sand?0xe6c485:c.kind==K::Ash?0xb6bcc5:0xbba389;
        for(int i=0;i<3;++i)p.line(7,16+i*13,57-i*4,16+i*13,1,color);
        for(int i=0;i<13;++i)p.box(7+(i*13+phase*2)%52,8+(i*7)%42,2+(i%2),2+(i%2),1,color);
    }else if(c.kind==K::Squall){
        for(int i=0;i<4;++i){const int y=13+i*10;p.line(6+(phase+i)%5,y,48,y,2,0xc9e8f1);p.line(48,y,55,y-3,2,0xc9e8f1);p.line(55,y-3,52,y-6,2,0xc9e8f1);}
    }else if(c.kind==K::Tornado){
        for(int i=0;i<6;++i){const int width=50-i*7,x=32-width/2+(phase+i)%3;p.line(x,9+i*7,x+width,9+i*7,3,0xd4dce8);}
    }
}

} // namespace stackchan::stage1
