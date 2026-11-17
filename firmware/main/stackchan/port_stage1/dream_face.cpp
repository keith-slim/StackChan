#include "port_compat.h"
#include "dream_face.h"
#include "dream_context.h"
#include "avatar_viewport.h"
#include "esp_random.h"
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace {
uint32_t RandomDreamInterval()
{
    constexpr uint32_t range=AvatarDreamFace::DREAM_MAX_INTERVAL_MS-AvatarDreamFace::DREAM_MIN_INTERVAL_MS+1;
    return AvatarDreamFace::DREAM_MIN_INTERVAL_MS+esp_random()%range;
}
bool Reached(uint32_t elapsed,uint32_t due){return static_cast<int32_t>(elapsed-due)>=0;}
}

AvatarDreamFace::AvatarDreamFace(lv_obj_t *parent)
{
    root_=lv_obj_create(parent);lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_,lv_pct(100),lv_pct(100));
    lv_obj_set_clickable(root_,false);lv_obj_set_scrollable(root_,false);
    scale_=stackchan::avatar::viewport_scale(parent);
    const auto px=[this](float n){return static_cast<int>(std::lround(n*scale_/avatar_config::SCALE_UNIT));};
    cloud_=lv_line_create(root_);symbol_=lv_line_create(root_);
    for(auto *line:{cloud_,symbol_}) {
        lv_obj_set_clickable(line,false);lv_obj_set_scrollable(line,false);
        lv_obj_set_style_line_color(line,lv_color_white(),0);
        lv_obj_set_style_line_width(line,std::max(1,px(2)),0);
        lv_obj_set_style_line_rounded(line,true,0);
    }
    constexpr float curves[][8]={
        {-30,14,-46,12,-46,-9,-29,-12},{-29,-12,-29,-30,-6,-37,8,-24},
        {8,-24,23,-31,40,-22,37,-9},{37,-9,54,-4,51,17,35,19},
        {35,19,29,34,7,34,-2,25},{-2,25,-14,33,-35,29,-30,14}};
    unsigned n=0;
    for(const auto &c:curves)for(int step=0;step<8;++step){
        const float t=step/8.0f,u=1-t;
        const float x=u*u*u*c[0]+3*u*u*t*c[2]+3*u*t*t*c[4]+t*t*t*c[6];
        const float y=u*u*u*c[1]+3*u*u*t*c[3]+3*u*t*t*c[5]+t*t*t*c[7];
        outline_[n++]={static_cast<lv_value_precise_t>(px((x+46)*0.62f)),static_cast<lv_value_precise_t>(px((y+37)*0.62f))};
    }
    outline_[48]=outline_[0];lv_line_set_points(cloud_,outline_,49);
    lv_obj_align(cloud_,LV_ALIGN_CENTER,px(95),px(-82));
    for(int i=0;i<2;++i){
        links_[i]=lv_obj_create(root_);lv_obj_remove_style_all(links_[i]);
        lv_obj_set_clickable(links_[i],false);lv_obj_set_scrollable(links_[i],false);
        lv_obj_set_style_bg_color(links_[i],lv_color_white(),0);lv_obj_set_style_bg_opa(links_[i],LV_OPA_COVER,0);
        lv_obj_set_style_radius(links_[i],LV_RADIUS_CIRCLE,0);
        lv_obj_set_size(links_[i],px(i?5:8),px(i?5:8));
        lv_obj_align(links_[i],LV_ALIGN_CENTER,px(i?69:77),px(i?-47:-56));
    }
    // 小さな線部品と塗りだけを共用し、食べ物用の画像・描画バッファを増やさない。
    for(auto &detail:details_){
        detail=lv_line_create(root_);lv_obj_set_clickable(detail,false);lv_obj_set_scrollable(detail,false);
        lv_obj_set_style_line_rounded(detail,true,0);
    }
    fill_=lv_obj_create(root_);lv_obj_remove_style_all(fill_);
    // 動物の顔も既存の塗り部品へ直接描き、画像バッファや常駐オブジェクトを増やさない。
    lv_obj_add_event_cb(fill_,DrawIllustration,LV_EVENT_DRAW_MAIN,this);
    lv_obj_set_clickable(fill_,false);lv_obj_set_scrollable(fill_,false);
    lv_obj_set_style_bg_opa(fill_,LV_OPA_COVER,0);lv_obj_set_style_radius(fill_,LV_RADIUS_CIRCLE,0);
    lv_obj_move_to_index(fill_,1); // 塗りは輪郭・骨・カップの取っ手より後ろへ置く。
    // コーヒーの液面とプリンのカラメルで、同じ小さな塗り部品を使う。
    topping_=lv_obj_create(root_);lv_obj_remove_style_all(topping_);
    lv_obj_set_clickable(topping_,false);lv_obj_set_scrollable(topping_,false);
    lv_obj_set_style_bg_opa(topping_,LV_OPA_COVER,0);lv_obj_set_style_bg_color(topping_,lv_color_hex(0x85502b),0);
    lv_obj_set_style_radius(topping_,LV_RADIUS_CIRCLE,0);lv_obj_move_to_index(topping_,2);
    for(auto *object:{details_[0],details_[1],fill_,topping_})lv_obj_set_hidden(object,true);
    Render(0);
}
AvatarDreamFace::~AvatarDreamFace(){lv_obj_delete(root_);}
void AvatarDreamFace::Render(uint32_t elapsed_ms)
{
    ClearIdea();
    if(status_symbol_>=0){status_symbol_=-1;motif_index_=MOTIF_COUNT;}
    if(avatar_dream::TalkLocked()){
        const auto motif=avatar_dream::TalkMotif();
        const auto index=static_cast<unsigned>(motif);
        lv_obj_set_hidden(root_,index>=MOTIF_COUNT);
        if(index<MOTIF_COUNT&&index!=motif_index_){motif_index_=index;DrawMotif(index);}
        return;
    }
    // 初回を含め、表示開始から次の表示開始まで15〜60秒。夢は毎回10秒間表示する。
    if(last_dream_elapsed_ms_==UINT32_MAX||elapsed_ms<last_dream_elapsed_ms_){
        next_dream_ms_=elapsed_ms+RandomDreamInterval();hide_dream_ms_=UINT32_MAX;dream_visible_=false;
        avatar_dream::Hide();avatar_dream::ClearSuppression();
    }
    last_dream_elapsed_ms_=elapsed_ms;
    if(dream_visible_&&Reached(elapsed_ms,hide_dream_ms_)){
        dream_visible_=false;hide_dream_ms_=UINT32_MAX;avatar_dream::Hide();
        // 寝言で長く固定され、次の予定も過ぎた場合は解除直後の再表示を避ける。
        if(Reached(elapsed_ms,next_dream_ms_))next_dream_ms_=elapsed_ms+RandomDreamInterval();
        avatar_dream::ClearSuppression();
    }
    if(dream_visible_){lv_obj_set_hidden(root_,avatar_dream::Suppressed());return;}
    lv_obj_set_hidden(root_,true);avatar_dream::Hide();
    if(avatar_dream::Suppressed()){
        avatar_dream::ClearSuppression();
        if(Reached(elapsed_ms,next_dream_ms_))next_dream_ms_=elapsed_ms+RandomDreamInterval();
        return;
    }
    if(!Reached(elapsed_ms,next_dream_ms_))return;
    const unsigned next=esp_random()%MOTIF_COUNT; // 従来の7種類とゴールデンレトリバー・猫から選ぶ。
    dream_visible_=true;hide_dream_ms_=elapsed_ms+DREAM_DISPLAY_MS;next_dream_ms_=elapsed_ms+RandomDreamInterval();
    lv_obj_set_hidden(root_,false);
    avatar_dream::Observe(next); // 睡眠中に実際に表示した絵柄だけを寝言へ渡す。
    if(next==motif_index_)return;
    motif_index_=next;DrawMotif(next);
}
void AvatarDreamFace::RenderMotif(unsigned motif)
{
    ClearIdea();
    if(motif>=MOTIF_COUNT){lv_obj_set_hidden(root_,true);return;}
    lv_obj_set_hidden(root_,false);
    if(status_symbol_<0&&motif_index_==motif)return;
    status_symbol_=-1;motif_index_=motif;DrawMotif(motif);
}
void AvatarDreamFace::ClearIdea()
{
    if(!idea_bulb_)return;
    idea_bulb_=false;status_symbol_=-1;motif_index_=MOTIF_COUNT;
}
void AvatarDreamFace::RenderIdeaBulb(uint32_t elapsed_ms)
{
    lv_obj_set_hidden(root_,false);
    const bool fresh=!idea_bulb_;
    if(fresh){
        idea_bulb_=true;status_symbol_=-1;motif_index_=MOTIF_COUNT;
        for(auto *object:{symbol_,details_[0],details_[1],topping_})lv_obj_set_hidden(object,true);
        const auto px=[this](int n){return stackchan::avatar::scaled(n,scale_);};
        lv_obj_set_hidden(fill_,false);lv_obj_set_style_bg_opa(fill_,LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(fill_,0,0);
        lv_obj_set_size(fill_,px(34),px(34));lv_obj_align(fill_,LV_ALIGN_CENTER,px(95),px(-82));
        // 絵柄の大きさが変わる時は雲の輪郭もまとめて再描画し、境界のAAを残さない。
        lv_obj_invalidate(cloud_);
    }
    // ガラスは消さずに明るさを変える。位相が同じUI更新では再描画を増やさない。
    const auto phase=static_cast<uint8_t>((elapsed_ms/180)%4);
    if(fresh||phase!=idea_phase_){idea_phase_=phase;lv_obj_invalidate(fill_);}
}
void AvatarDreamFace::DrawMotif(unsigned next)
{
    // 小芝居の初回はまだ雲の割合指定が未計算なので、線と塗りを置く前に寸法を確定する。
    lv_obj_update_layout(root_);
    if(next==5||next==6){DrawStatus(next==5?AvatarCloudSymbol::Construction:AvatarCloudSymbol::Mask);return;}
    const auto px=[this](float n){return static_cast<lv_value_precise_t>(std::lround(n*scale_/avatar_config::SCALE_UNIT));};
    if(next>=7){
        for(auto *object:{symbol_,details_[0],details_[1],topping_})lv_obj_set_hidden(object,true);
        lv_obj_set_hidden(fill_,false);lv_obj_set_style_bg_opa(fill_,LV_OPA_TRANSP,0);
        lv_obj_set_style_border_width(fill_,0,0);
        lv_obj_set_size(fill_,px(36),px(30));lv_obj_align(fill_,LV_ALIGN_CENTER,px(95),px(-81));
        lv_obj_invalidate(fill_); // 犬と猫で大きさが同じでも、顔の描画内容は必ず更新する。
        return;
    }
    lv_obj_set_hidden(symbol_,false);lv_obj_set_style_bg_opa(fill_,LV_OPA_COVER,0);
    for(auto *object:{details_[0],details_[1],fill_})lv_obj_set_hidden(object,next<2);
    lv_obj_set_hidden(topping_,next<3);
    lv_obj_set_style_border_width(fill_,0,0);
    lv_obj_set_style_line_width(symbol_,std::max(1,static_cast<int>(px(2))),0);
    if(next>=2){
        // 三本の線は32×28の共通座標で置き、部品ごとの自動中央揃えによる位置ずれを防ぐ。
        const int x=lv_obj_get_width(root_)/2+px(95-16),y=lv_obj_get_height(root_)/2+px(-81-14);
        const auto path=[&](lv_obj_t *object,lv_point_precise_t *storage,const auto &points,lv_color_t color){
            unsigned count=0;for(const auto &point:points)storage[count++]={px(point[0]),px(point[1])};
            lv_line_set_points(object,storage,count);lv_obj_align(object,LV_ALIGN_TOP_LEFT,x,y);
            lv_obj_set_style_line_color(object,color,0);lv_obj_set_style_line_width(object,std::max(1,static_cast<int>(px(2))),0);
        };
        if(next==2){
            constexpr float left[][2]={{9,12},{5,12},{4,9},{2,9},{1,11},{2,14},{1,17},{2,19},{4,19},{5,16},{9,16}};
            constexpr float right[][2]={{23,12},{27,12},{28,9},{30,9},{31,11},{30,14},{31,17},{30,19},{28,19},{27,16},{23,16}};
            constexpr float glaze[][2]={{13,7},{11,10},{11,13}};
            path(details_[0],detail_points_[0],left,lv_color_white());path(details_[1],detail_points_[1],right,lv_color_white());
            path(symbol_,motif_,glaze,lv_color_hex(0xffdfa5));
            lv_obj_set_style_radius(fill_,LV_RADIUS_CIRCLE,0);
            lv_obj_set_size(fill_,px(18),px(22));lv_obj_align(fill_,LV_ALIGN_CENTER,px(95),px(-81));
            lv_obj_set_style_bg_color(fill_,lv_color_hex(0xb76d38),0);
            lv_obj_set_style_border_color(fill_,lv_color_hex(0xf4b56a),0);lv_obj_set_style_border_width(fill_,std::max(1,static_cast<int>(px(1))),0);
        }else if(next==3){
            constexpr float cup[][2]={{4,8},{24,8},{23,20},{21,23},{8,23},{6,20},{4,8}};
            constexpr float handle[][2]={{24,10},{28,10},{30,12},{30,16},{27,19},{24,19}};
            constexpr float steam[][2]={{14,6},{11,4},{12,1},{15,0}};
            path(symbol_,motif_,cup,lv_color_white());path(details_[0],detail_points_[0],handle,lv_color_white());
            path(details_[1],detail_points_[1],steam,lv_color_hex(0xcccccc));
            lv_obj_set_size(fill_,px(19),px(15));lv_obj_align(fill_,LV_ALIGN_CENTER,px(93),px(-79));
            lv_obj_set_style_radius(fill_,px(4),0);lv_obj_set_style_bg_color(fill_,lv_color_hex(0xfff1da),0);
            lv_obj_set_size(topping_,px(18),px(4));lv_obj_align(topping_,LV_ALIGN_CENTER,px(93),px(-85));
        }else{
            // 小さな雲でも判別できるよう、黄色い台形・茶色いカラメル・白い皿と銀のスプーンを分ける。
            unsigned count=0;
            for(int row=0;row<12;++row){
                const float half=6+3*row/11.0f;
                const float left=12-half,right=12+half;
                motif_[count++]={px(row%2?right:left),px(12+row)};
                motif_[count++]={px(row%2?left:right),px(12+row)};
            }
            lv_line_set_points(symbol_,motif_,count);lv_obj_align(symbol_,LV_ALIGN_TOP_LEFT,x,y);
            lv_obj_set_style_line_color(symbol_,lv_color_hex(0xffd467),0);
            lv_obj_set_style_line_width(symbol_,std::max(1,static_cast<int>(px(2))),0);
            constexpr float handle[][2]={{28,11},{25,25}};
            constexpr float bowl[][2]={{28,1},{30,2},{31,4},{31,7},{30,10},{28,11},{26,10},{25,7},{25,4},{26,2},{28,1}};
            path(details_[0],detail_points_[0],handle,lv_color_hex(0xd7e1eb));
            path(details_[1],detail_points_[1],bowl,lv_color_hex(0xeaf0f5));
            lv_obj_set_size(fill_,px(25),px(4));lv_obj_align(fill_,LV_ALIGN_CENTER,px(91),px(-70));
            lv_obj_set_style_radius(fill_,LV_RADIUS_CIRCLE,0);lv_obj_set_style_bg_color(fill_,lv_color_hex(0xfff6e5),0);
            lv_obj_set_size(topping_,px(14),px(4));lv_obj_align(topping_,LV_ALIGN_CENTER,px(91),px(-85));
        }
        return;
    }
    unsigned count=0;
    if(next==0){
        for(int i=0;i<32;++i){const float t=i*6.283185307f/32;
            const float s=std::sin(t);
            motif_[count++]={px(13+12*s*s*s),px(13-(10*std::cos(t)-4*std::cos(2*t)-2*std::cos(3*t)-std::cos(4*t)))};
        }
    }else for(int i=0;i<10;++i){const float t=-1.570796327f+i*3.141592654f/5,r=i%2?5:13;
        motif_[count++]={px(13+r*std::cos(t)),px(13+r*std::sin(t))};}
    motif_[count]=motif_[0];lv_line_set_points(symbol_,motif_,count+1);
    lv_obj_set_style_line_color(symbol_,next?lv_color_hex(0xffd56a):lv_color_hex(0xff9eae),0);
    lv_obj_align(symbol_,LV_ALIGN_CENTER,px(95),px(-81));
}

// 状態表示も夢も同じ8オブジェクトを使い、新しい画像や全画面バッファを確保しない。
void AvatarDreamFace::DrawStatus(AvatarCloudSymbol symbol)
{
    // 作成直後も親の寸法を確定し、中央揃えの塗りと絶対座標の線が離れるのを防ぐ。
    lv_obj_update_layout(root_);
    lv_obj_set_hidden(symbol_,false);lv_obj_set_style_bg_opa(fill_,LV_OPA_COVER,0);
    const auto px=[this](float n){return static_cast<lv_value_precise_t>(std::lround(n*scale_/avatar_config::SCALE_UNIT));};
    const int x=lv_obj_get_width(root_)/2+px(79),y=lv_obj_get_height(root_)/2+px(-95);
    const auto path=[&](lv_obj_t *object,lv_point_precise_t *storage,const auto &points,lv_color_t color,int width=2,int offset_x=0){
        unsigned count=0;for(const auto &point:points)storage[count++]={px(point[0]),px(point[1])};
        lv_line_set_points(object,storage,count);lv_obj_align(object,LV_ALIGN_TOP_LEFT,x+px(offset_x),y);
        lv_obj_set_style_line_color(object,color,0);lv_obj_set_style_line_width(object,std::max(1,static_cast<int>(px(width))),0);
    };
    for(auto *object:{details_[0],details_[1],fill_})lv_obj_set_hidden(object,false);
    lv_obj_set_hidden(topping_,true);lv_obj_set_style_border_width(fill_,0,0);
    if(symbol==AvatarCloudSymbol::Construction){
        // 黄色い工事柵に黒の斜線と二本脚を付け、黒背景でも判別できるようにする。
        constexpr float outline[][2]={{2,7},{30,7},{30,19},{2,19},{2,7}};
        constexpr float stripes[][2]={{3,17},{10,8},{10,17},{17,8},{17,17},{24,8},{24,17},{29,10}};
        constexpr float legs[][2]={{7,20},{7,27},{7,20},{25,20},{25,27}};
        path(symbol_,motif_,outline,lv_color_white());path(details_[0],detail_points_[0],stripes,lv_color_black(),3);path(details_[1],detail_points_[1],legs,lv_color_white());
        lv_obj_set_size(fill_,px(28),px(12));lv_obj_align(fill_,LV_ALIGN_CENTER,px(95),px(-82));
        lv_obj_set_style_radius(fill_,px(1),0);lv_obj_set_style_bg_color(fill_,lv_color_hex(0xffd56a),0);
    }else{
        // 水色のマスク・白いひだ・左右の耳ひもを線と塗りだけで描く。
        constexpr float mask[][2]={{5,7},{27,7},{27,21},{5,21},{5,7},{5,11},{27,11},{27,16},{5,16}};
        // 本体の枠と見分けられるよう、耳ひもは左右へ広げて少し太くする。
        constexpr float left[][2]={{9,8},{4,8},{1,10},{0,14},{1,18},{4,20},{9,20}};
        constexpr float right[][2]={{31,8},{36,8},{39,10},{40,14},{39,18},{36,20},{31,20}};
        path(symbol_,motif_,mask,lv_color_white());path(details_[0],detail_points_[0],left,lv_color_white(),3,-4);path(details_[1],detail_points_[1],right,lv_color_white(),3,-4);
        lv_obj_set_size(fill_,px(22),px(14));lv_obj_align(fill_,LV_ALIGN_CENTER,px(95),px(-81));
        lv_obj_set_style_radius(fill_,px(3),0);lv_obj_set_style_bg_color(fill_,lv_color_hex(0x7fdded),0);
    }
}

void AvatarDreamFace::DrawIllustration(lv_event_t *event)
{
    auto *self=static_cast<AvatarDreamFace *>(lv_event_get_user_data(event));
    if(!self->idea_bulb_&&(self->status_symbol_>=0||self->motif_index_<7||self->motif_index_>=MOTIF_COUNT))return;
    auto *layer=lv_event_get_layer(event);
    lv_area_t bounds;lv_obj_get_coords(self->fill_,&bounds);
    const auto px=[self](int n){return stackchan::avatar::scaled(n,self->scale_);};
    const auto point=[&](int x,int y){return lv_point_precise_t{
        static_cast<lv_value_precise_t>(bounds.x1+px(x)),static_cast<lv_value_precise_t>(bounds.y1+px(y))};};
    const auto rounded=[&](int x1,int y1,int x2,int y2,int radius,uint32_t color,uint32_t edge=0){
        lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.radius=px(radius);d.bg_opa=LV_OPA_COVER;
        d.bg_color=lv_color_hex(color);d.border_width=edge?std::max(1,px(1)):0;d.border_color=lv_color_hex(edge);
        const lv_area_t area={bounds.x1+px(x1),bounds.y1+px(y1),bounds.x1+px(x2),bounds.y1+px(y2)};
        lv_draw_rect(layer,&d,&area);
    };
    const auto stroke=[&](std::initializer_list<std::pair<int,int>> points,uint32_t color){
        lv_draw_line_dsc_t d;lv_draw_line_dsc_init(&d);d.width=std::max(1,px(1));
        d.color=lv_color_hex(color);d.round_start=d.round_end=1;
        auto previous=points.begin();for(auto current=previous+1;current!=points.end();++current){
            d.p1=point(previous->first,previous->second);d.p2=point(current->first,current->second);
            lv_draw_line(layer,&d);previous=current;
        }
    };
    const auto triangle=[&](int x1,int y1,int x2,int y2,int x3,int y3,uint32_t color){
        lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);d.color=lv_color_hex(color);
        d.p[0]=point(x1,y1);d.p[1]=point(x2,y2);d.p[2]=point(x3,y3);lv_draw_triangle(layer,&d);
    };
    if(self->idea_bulb_){
        // 小さな雲でも電球と分かるよう、常時点灯するガラス・首・金属口金を分ける。
        constexpr uint32_t glass[]={0xffdd69,0xffe88d,0xfff3b2,0xffe88d};
        constexpr uint32_t ray[]={0xd9ae3d,0xffd65c,0xffee9a,0xffd65c};
        const bool bright=self->idea_phase_==2;
        const auto light=ray[self->idea_phase_];
        stroke({{17,4},{17,bright?1:2}},light);
        stroke({{7,7},{bright?4:5,bright?4:5}},light);
        stroke({{5,14},{bright?1:2,14}},light);
        stroke({{27,7},{bright?30:29,bright?4:5}},light);
        stroke({{29,14},{bright?32:31,14}},light);
        stroke({{7,21},{bright?4:5,bright?24:23}},light);
        stroke({{27,21},{bright?30:29,bright?24:23}},light);
        rounded(9,6,25,22,8,glass[self->idea_phase_],0xffd35d);
        rounded(12,19,22,27,3,glass[self->idea_phase_]);
        rounded(12,9,14,14,2,0xffffe6);
        stroke({{13,17},{17,21},{21,17}},0xc4912e);
        stroke({{17,21},{17,26}},0xc4912e);
        rounded(12,26,22,31,2,0xc6ced8,0x8e9bad);
        stroke({{13,28},{21,28}},0x6c7b8e);
        stroke({{13,30},{21,30}},0x6c7b8e);
        rounded(15,31,19,33,2,0x8e9bad);
        return;
    }
    if(self->motif_index_==7){
        // ゴールデンレトリバー：金色の長い垂れ耳、丸い鼻、明るい口元と小さな舌。
        rounded(1,5,10,25,5,0xce914d,0x9b662e);rounded(26,5,35,25,5,0xce914d,0x9b662e);
        rounded(5,2,31,26,11,0xf1ca7d,0xc18c4b);
        rounded(9,15,19,25,5,0xfff0cf);rounded(17,15,27,25,5,0xfff0cf);
        rounded(11,10,13,13,2,0x3d2c1c);rounded(23,10,25,13,2,0x3d2c1c);
        rounded(11,10,11,10,0,0xffffff);rounded(23,10,23,10,0,0xffffff);
        rounded(16,21,20,26,2,0xf18b98);
        stroke({{13,19},{15,22},{18,22},{21,22},{23,19}},0x6b4530);
        stroke({{18,17},{18,21}},0x6b4530);
        rounded(15,14,21,18,3,0x4b3023);
        stroke({{3,13},{3,18}},0xe2af69);stroke({{33,13},{33,18}},0xe2af69);
    }else{
        // 小さな雲でもやさしく見えるよう、三角耳・丸い瞳・桃色の頬のシンプルな猫にする。
        triangle(5,1,5,18,17,10,0xbb9c82);triangle(31,1,31,18,19,10,0xbb9c82);
        triangle(6,3,6,16,15,10,0xfff0df);triangle(30,3,30,16,21,10,0xfff0df);
        triangle(8,6,8,12,12,10,0xf0a3b2);triangle(28,6,28,12,24,10,0xf0a3b2);
        rounded(5,8,31,27,10,0xfff0df,0xbb9c82);
        rounded(7,19,12,21,2,0xf7b6be);rounded(24,19,29,21,2,0xf7b6be);
        rounded(10,13,14,18,3,0x4b3b35);rounded(22,13,26,18,3,0x4b3b35);
        rounded(11,14,12,15,1,0xffffff);rounded(23,14,24,15,1,0xffffff);
        stroke({{18,20},{18,22}},0x806253);
        stroke({{14,22},{16,24},{18,22},{20,24},{22,22}},0x806253);
        triangle(16,19,20,19,18,21,0xe38b9b);
        stroke({{1,18},{8,19}},0xbda28f);stroke({{1,22},{8,21}},0xbda28f);
        stroke({{28,19},{35,18}},0xbda28f);stroke({{28,21},{35,22}},0xbda28f);
    }
}
void AvatarDreamFace::RenderStatus(AvatarCloudSymbol symbol)
{
    ClearIdea();
    lv_obj_set_hidden(root_,false);
    if(status_symbol_==static_cast<int>(symbol))return;
    status_symbol_=static_cast<int>(symbol);motif_index_=MOTIF_COUNT;
    DrawStatus(symbol);
}
