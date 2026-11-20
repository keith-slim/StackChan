#pragma once
#include "yawn_animation.h"
namespace avatar_nod {
constexpr uint32_t DURATION_MS=420; // 聞き終わりに一度だけ、小さくうなずく。通信とは独立した描画時間。
inline int Offset(uint32_t elapsed)
{
    if(elapsed>=DURATION_MS)return 0;
    const auto half=DURATION_MS/2;
    const float amount=elapsed<half?avatar_yawn::Ease(elapsed,half):1-avatar_yawn::Ease(elapsed-half,half);
    // 移植元の位置は±100を±16pxへ変換するため、38で基準画面の約6pxになる。
    return static_cast<int>(amount*38+0.5f);
}
}
