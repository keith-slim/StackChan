#pragma once
// Ported from S3-stackchan: network-free weather-change policy.
// Reuses StackChan's existing weather conditions and Live speech transport.
#include "weather_conditions.h"
#include <cstdint>

namespace stackchan::stage1::weather {
// 天気は取得済み分類だけで比較する。新しい取得要求・文字列の常駐コピーは不要。
class Changes {
public:
    static constexpr int64_t COOLDOWN_US=30LL*60*1000000;
    static constexpr int64_t PENDING_MAX_US=5LL*60*1000000;
    void Observe(const Appearance &appearance,int64_t now,bool active)
    {
        if(!appearance.visible||appearance.condition.kind==Kind::Unknown){known_=pending_=false;return;}
        const auto current=appearance.condition;
        if(!known_||!active){observed_=current;known_=true;pending_=false;return;}
        if(!Same(observed_,current)){
            if(!pending_)from_=observed_;
            to_=current;observed_=current;
            pending_=!Same(from_,to_);changed_us_=now;
        }
        if(pending_&&(now<changed_us_||now-changed_us_>=PENDING_MAX_US))pending_=false;
    }
    bool Ready(int64_t now)const
    {
        return pending_&&(!announced_||(now>=spoken_us_&&now-spoken_us_>=COOLDOWN_US));
    }
    // The current Xiaozhi API confirms admission, not final playback; use
    // admission for cooldown so transport failures never cause a retry storm.
    void Accepted(int64_t now){pending_=false;announced_=true;spoken_us_=now;}
    const char *Text()const
    {
        using K=Kind;
        const auto kind=to_.kind;
        if(kind==from_.kind){
            if(kind==K::Rain||kind==K::Drizzle||kind==K::FreezingRain){
                if(to_.strength>from_.strength)return "さっきより雨が強くなってきたみたい。";
                if(to_.strength<from_.strength)return "雨が少し弱くなってきたみたいだね。";
            }
            if(kind==K::Snow||kind==K::Sleet){
                if(to_.strength>from_.strength)return "さっきより雪が強くなってきたみたい。";
                if(to_.strength<from_.strength)return "雪が少し弱くなってきたみたいだね。";
            }
            if(kind==K::Clouds)return to_.clouds>from_.clouds?"雲が多くなってきたみたい。":"雲が少なくなってきたみたいだね。";
        }
        switch(kind){
        case K::Clear:return "晴れてきたみたい。なんだか気持ちがいいね。";
        case K::Clouds:return to_.clouds<=2?"少し雲が出てきたみたいだね。":"曇ってきたみたいだね。";
        case K::Drizzle:return "霧雨が降ってきたみたいだね。";
        case K::Rain:return to_.strength>=3?"強い雨になってきたみたい。外に出るなら気をつけてね。":"雨が降ってきたみたいだね。";
        case K::FreezingRain:return "凍る雨になっているみたい。足元に気をつけてね。";
        case K::Thunderstorm:return "雷のある天気になってきたみたいだね。";
        case K::Snow:return to_.strength>=4?"雪が強くなってきたみたい。外に出るなら気をつけてね。":"雪が降ってきたみたいだね。";
        case K::Sleet:return "みぞれになってきたみたいだね。";
        case K::Mist:case K::Fog:return "もやや霧が出てきたみたいだね。";
        case K::Smoke:return "煙のある天気情報になっているみたい。";
        case K::Haze:return "空がかすんできたみたいだね。";
        case K::Dust:case K::Sand:return "砂やほこりのある天気になっているみたい。";
        case K::Ash:return "火山灰の情報が出ているみたいだね。";
        case K::Squall:return "突風のある天気になっているみたいだね。";
        case K::Tornado:return "竜巻の天気情報が出ているみたい。最新の情報を確認してね。";
        default:return "天気が変わってきたみたいだね。";
        }
    }
private:
    static bool Same(const Condition &a,const Condition &b)
    {
        return a.kind==b.kind&&a.strength==b.strength&&a.clouds==b.clouds&&
               a.lightning==b.lightning&&a.showers==b.showers;
    }
    Condition observed_,from_,to_;
    int64_t changed_us_=0,spoken_us_=0;
    bool known_=false,pending_=false,announced_=false;
};
} // namespace stackchan::stage1::weather
