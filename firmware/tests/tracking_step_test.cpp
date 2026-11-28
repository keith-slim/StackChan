#include "tracking_step.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
using namespace stackchan::motion;
static void check(bool ok) { if (!ok) std::abort(); }
int main() {
    auto a = AdvanceTracking(0,0,1000,-1280,1280,200,35.6f,11.93f,20);
    check(a.position > 0 && a.position <= 4 && std::abs(a.velocity) <= 200);
    auto b = AdvanceTracking(0,200,1000,-1280,1280,200,35.6f,11.93f,10000);
    check(b.position <= 12.001f && !b.reached);
    auto c = AdvanceTracking(0,200,-1000,-1280,1280,200,35.6f,11.93f,20);
    check(c.velocity < 200 && c.velocity >= -200);
    auto d = AdvanceTracking(1279,200,2000,-1280,1280,200,35.6f,11.93f,20);
    check(d.position == 1280 && d.reached && d.velocity == 0);
    auto e = AdvanceTracking(0,0,-100,-1280,1280,200,35.6f,11.93f,20);
    check(e.position < 0 && e.position >= -4);
    float pos=0, vel=0;
    bool reached=false;
    for(int i=0;i<3000&&!reached;++i) {
        auto s=AdvanceTracking(pos,vel,600,-1280,1280,200,35.6f,11.93f,20);
        check(s.position>=pos && s.position<=600 && std::abs(s.velocity)<=200);
        pos=s.position; vel=s.velocity; reached=s.reached;
    }
    check(reached && pos==600);
    std::puts("tracking_step_test: passed");
}
