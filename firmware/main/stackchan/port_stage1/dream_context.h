#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

// 描画済みの夢だけを小さな値で共有する。ワーカーからLVGLの部品を参照しない。
namespace avatar_dream {
enum class Motif : uint8_t { Heart, Star, Meat, Coffee, Pudding, Construction, Mask, Dog, Cat, Car, None=255 };
inline constexpr unsigned COUNT=10;
namespace detail {
inline constexpr unsigned ACTIVE=0x100, VISIBLE=0x200, TALK_LOCK=0x400, SUPPRESSED=0x800;
inline std::atomic<unsigned> state{static_cast<unsigned>(Motif::None)};
}
// 昼寝の開始時にだけ有効にし、前の昼寝や状態表示の雲を混ぜない。
inline void Begin() { detail::state.store(detail::ACTIVE|static_cast<unsigned>(Motif::None)); }
inline void Observe(unsigned motif)
{
    if(motif>=COUNT)return;
    auto previous=detail::state.load();
    while((previous&detail::ACTIVE)&&!(previous&detail::TALK_LOCK)){
        const auto next=(previous&~(0xffU|detail::SUPPRESSED))|detail::VISIBLE|motif;
        if(detail::state.compare_exchange_weak(previous,next))break;
    }
}
// 描画周期の空白では最終絵柄を履歴へ残しつつ、現在表示中ではないことだけを共有する。
inline void Hide()
{
    auto previous=detail::state.load();
    while((previous&detail::ACTIVE)&&!(previous&detail::TALK_LOCK)&&(previous&detail::VISIBLE))
        if(detail::state.compare_exchange_weak(previous,previous&~detail::VISIBLE))break;
}
inline Motif Last()
{
    const auto value=detail::state.load();
    return (value&detail::ACTIVE)&&(value&0xff)<COUNT?static_cast<Motif>(value&0xff):Motif::None;
}
inline Motif Visible()
{
    const auto value=detail::state.load();
    return (value&(detail::ACTIVE|detail::VISIBLE))==(detail::ACTIVE|detail::VISIBLE)&&(value&0xff)<COUNT
        ?static_cast<Motif>(value&0xff):Motif::None;
}
// 寝言要求時の表示状態を固定する。夢なしも固定し、AI待ち中に別の夢を寝言へ結び付けない。
inline Motif BeginTalk()
{
    auto previous=detail::state.load();
    while(previous&detail::ACTIVE){
        const auto next=previous|detail::TALK_LOCK;
        if(detail::state.compare_exchange_weak(previous,next))
            return (next&detail::VISIBLE)&&(next&0xff)<COUNT?static_cast<Motif>(next&0xff):Motif::None;
    }
    return Motif::None;
}
inline bool TalkLocked(){return (detail::state.load()&detail::TALK_LOCK)!=0;}
inline Motif TalkMotif()
{
    const auto value=detail::state.load();
    return (value&(detail::TALK_LOCK|detail::VISIBLE))==(detail::TALK_LOCK|detail::VISIBLE)&&(value&0xff)<COUNT
        ?static_cast<Motif>(value&0xff):Motif::None;
}
// 吹き出しと同時に夢を消し、同じ表示周期の途中で即座に再表示しない。
inline void EndTalk()
{
    auto previous=detail::state.load();
    while((previous&detail::ACTIVE)&&(previous&detail::TALK_LOCK)){
        const auto next=(previous&~(detail::TALK_LOCK|detail::VISIBLE))|detail::SUPPRESSED;
        if(detail::state.compare_exchange_weak(previous,next))break;
    }
}
inline bool Suppressed(){return (detail::state.load()&detail::SUPPRESSED)!=0;}
inline void ClearSuppression()
{
    auto previous=detail::state.load();
    while(previous&detail::SUPPRESSED)
        if(detail::state.compare_exchange_weak(previous,previous&~detail::SUPPRESSED))break;
}
// 起床時は最後の絵柄を一度だけ引き取り、以降の読取りからは除外する。
inline Motif End()
{
    const auto value=detail::state.exchange(static_cast<unsigned>(Motif::None));
    return (value&detail::ACTIVE)&&(value&0xff)<COUNT?static_cast<Motif>(value&0xff):Motif::None;
}
inline const char *Name(Motif motif)
{
    static constexpr const char *names[]={"ハート","星","骨付き肉","コーヒー","プリン","工事現場","マスク","犬","猫","車"};
    const auto index=static_cast<unsigned>(motif);return index<COUNT?names[index]:nullptr;
}
inline const char *Remark(Motif motif)
{
    static constexpr const char *lines[]={
        "なんだか、幸せな夢だったよ。","星を見に行く夢だったよ。","大きなお肉の夢を見てたよ。",
        "コーヒーのいい香りがする夢だったよ。","プリン、あとひとくちだったのに。",
        "夢の中でも工事中だったよ。","夢の中でマスクを探してたよ。","わんこと遊ぶ夢だったよ。","ねことお昼寝する夢だったよ。","車でどこかへ出かける夢だったよ。"};
    const auto index=static_cast<unsigned>(motif);return index<COUNT?lines[index]:nullptr;
}
}
