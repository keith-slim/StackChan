#include "stage1_clock.h"
#include <cassert>
using namespace stackchan::stage1;
int main() {
    Clock c;
    assert(Clock::IsDay(359,c.GetConfig())==false);
    assert(Clock::IsDay(360,c.GetConfig())==true);
    assert(Clock::IsDay(1379,c.GetConfig())==true);
    assert(Clock::IsDay(1380,c.GetConfig())==false);
    Config bad = c.GetConfig(); bad.wake_minute=bad.sleep_minute;
    assert(!c.SetConfig(bad));
    Config cross=c.GetConfig();cross.wake_minute=1320;cross.sleep_minute=360;
    assert(c.SetConfig(cross));assert(Clock::IsDay(10,cross));assert(!Clock::IsDay(1000,cross));
    c.Reset();
    auto s=c.Step({1,1000,true,true,false,0});assert(s.phase==Phase::NightSleep);
    s=c.Step({2,1400,true,true,false,0});assert(s.phase==Phase::MorningWake);
    s=c.Step({3000,1400,true,true,false,0});assert(s.phase==Phase::Awake);
    c.SetConfig(Config{}); c.Reset();
    s=c.Step({1,500,true,true,false,1}); assert(s.phase==Phase::Awake);
    s=c.Step({2,1400,true,false,false,1}); assert(s.phase==Phase::NightPending);
    s=c.Step({3,1400,true,true,false,1}); assert(s.phase==Phase::NightDrowsy);
    s=c.Step({10003,1400,true,true,false,1});assert(s.phase==Phase::NightYawn);
    s=c.Step({13503,1400,true,true,false,1});assert(s.phase==Phase::NightSleep);
    s=c.Step({13504,1400,true,false,false,1});assert(s.phase==Phase::NightTemporary);
    s=c.Step({13505,1400,true,true,true,1});assert(s.phase==Phase::NightTemporary);
    s=c.Step({28504,1400,true,true,false,1});assert(s.phase==Phase::NightTemporary);
    s=c.Step({28505,1400,true,true,false,1});assert(s.phase==Phase::NightSleep);
    c.Reset();
    s=c.Step({0,500,true,true,false,1});assert(s.phase==Phase::Awake);
    s=c.Step({1,500,true,true,false,1});assert(s.phase==Phase::Awake);
    s=c.Step({90003,500,true,true,false,1});assert(s.phase==Phase::Theatre);
    assert(s.kind != Kind::None);
    s=c.Step({90004,500,true,true,true,1});assert(s.phase==Phase::Awake);
    s=c.Step({90005,500,true,true,false,1});assert(s.phase==Phase::Awake);
    // Cross a uint32 wrap during a short sleep animation, no subtraction underflow.
    c.Reset();s=c.Step({0xffffff00u,500,true,true,false,1});
    s=c.Step({0xffffff01u,1400,true,true,false,1});assert(s.phase==Phase::NightDrowsy);
    s=c.Step({10000-255u,1400,true,true,false,1});assert(s.phase==Phase::NightYawn);

    Clock naps;
    Config nap_cfg{};
    nap_cfg.theatre = false;
    nap_cfg.nap_chance = 100;
    assert(naps.SetConfig(nap_cfg));
    s=naps.Step({0,500,true,true,false,0}); assert(s.phase==Phase::Awake);
    s=naps.Step({1,500,true,true,false,0}); assert(s.phase==Phase::Awake);
    s=naps.Step({600002,500,true,true,false,0}); assert(s.phase==Phase::NapDrowsy);
    s=naps.Step({626502,500,true,true,false,0}); assert(s.phase==Phase::NapYawn);
    s=naps.Step({630002,500,true,true,false,0}); assert(s.phase==Phase::NapSleep);
    s=naps.Step({645002,500,true,true,false,0}); assert(s.phase==Phase::NapSleep && s.dream>=0);
    s=naps.Step({645003,500,true,false,true,0}); assert(s.phase==Phase::Awake);
}
