#pragma once
#include "lvgl.h"
#include <cstdint>
enum class AvatarCloudSymbol { Construction, Mask };
// 昼寝の夢とマイク復旧で、同じ雲と小さな線部品を共用する。
class AvatarDreamFace {
public:
    static constexpr unsigned MOTIF_COUNT=9; // 犬・猫を含む夢の絵柄数。抽選と描画で共用する。
    static constexpr uint32_t DREAM_MIN_INTERVAL_MS=15000;
    static constexpr uint32_t DREAM_MAX_INTERVAL_MS=60000;
    static constexpr uint32_t DREAM_DISPLAY_MS=10000;
    explicit AvatarDreamFace(lv_obj_t *parent);
    ~AvatarDreamFace();
    void Render(uint32_t elapsed_ms);
    // 待機中の芝居でも同じ絵柄を使う。実際の昼寝の夢としては記録しない。
    void RenderMotif(unsigned motif);
    // ひらめき専用。夢の星は変えず、既存部品で点灯と光線の明滅だけを描く。
    void RenderIdeaBulb(uint32_t elapsed_ms);
    // 状態表示は夢の表示周期と独立し、解除まで同じ雲を表示する。
    void RenderStatus(AvatarCloudSymbol symbol);
private:
    lv_obj_t *root_, *cloud_, *links_[2], *symbol_, *details_[2], *fill_, *topping_;
    lv_point_precise_t outline_[49]={}, motif_[33]={}, detail_points_[2][16]={};
    void DrawStatus(AvatarCloudSymbol symbol);
    void DrawMotif(unsigned motif);
    void ClearIdea(); // モード切替で電球の透明な塗り・位相を持ち越さない。
    static void DrawIllustration(lv_event_t *event);
    bool idea_bulb_=false;
    uint8_t idea_phase_=0;
    int status_symbol_=-1;
    int scale_;
    uint32_t next_dream_ms_=UINT32_MAX;
    uint32_t hide_dream_ms_=UINT32_MAX;
    uint32_t last_dream_elapsed_ms_=UINT32_MAX;
    bool dream_visible_=false;
    unsigned motif_index_=MOTIF_COUNT;
};
