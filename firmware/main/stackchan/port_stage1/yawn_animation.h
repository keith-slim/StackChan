#pragma once
#include <cstdint>
#include <cmath>

namespace avatar_yawn {
// 採用したプレビューと同じ時間配分を、入眠の状態管理と描画で共有する。
constexpr uint32_t DROWSY_MS=600, OPEN_MS=1200, HOLD_MS=800, CLOSE_MS=900;
constexpr uint32_t DURATION_MS=DROWSY_MS+OPEN_MS+HOLD_MS+CLOSE_MS;
struct Frame { bool drowsy; float opening; };
inline float Ease(uint32_t elapsed,uint32_t duration)
{
    return (1.0f-std::cos(3.141592654f*static_cast<float>(elapsed)/duration))*0.5f;
}
inline Frame At(uint32_t elapsed)
{
    if(elapsed<DROWSY_MS)return {true,Ease(elapsed,DROWSY_MS)};
    elapsed-=DROWSY_MS;
    if(elapsed<OPEN_MS)return {false,Ease(elapsed,OPEN_MS)};
    elapsed-=OPEN_MS;
    if(elapsed<HOLD_MS)return {false,1.0f};
    elapsed-=HOLD_MS;
    return {false,elapsed<CLOSE_MS?1.0f-Ease(elapsed,CLOSE_MS):0.0f};
}
}
