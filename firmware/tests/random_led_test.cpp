#include <hal/utils/rgb_led_controller.h>
#include <cassert>
#include <set>
#include <tuple>
using namespace stackchan::led;

int main() {
    RandomFade f;
    RandomFade::Frame initial{};
    f.Start(0xfffffff0u, 42, initial);
    assert(f.Colors() == initial);
    f.Tick(0xfffffff0u + 100u);
    std::set<std::tuple<int,int,int>> colors;
    for (auto c : f.Colors()) colors.emplace(c.r,c.g,c.b);
    assert(colors.size() > 6);
    for (uint32_t elapsed = 120; elapsed < 10000; elapsed += 20) {
        auto previous = f.Colors();
        f.Tick(0xfffffff0u + elapsed);
        for (unsigned i = 0; i < f.kCount; ++i) {
            auto c = f.Colors()[i];
            assert(c.r <= 168 && c.g <= 168 && c.b <= 168);
            auto delta = [](int a, int b) { return a > b ? a-b : b-a; };
            assert(delta(c.r,previous[i].r) <= 27);
            assert(delta(c.g,previous[i].g) <= 27);
            assert(delta(c.b,previous[i].b) <= 27);
        }
    }
    Controller c;
    Controller::Frame output{};
    unsigned batches=0, refreshes=0;
    auto flush = [&] { c.Flush([&](unsigned i,uint8_t r,uint8_t g,uint8_t b){ output[i]={r,g,b}; },
        [&]{ ++refreshes; }, [&](const auto& frame){output=frame;++batches;}); };
    auto tick = [&](uint32_t now, bool allowed=true, bool shutdown=false) {
        if (c.TickRandom(now,allowed,shutdown,[]{return 42u;})) flush();
    };
    const Rgb solid{12,34,56}, speech{100,0,0}, off{};
    for (unsigned i=0;i<c.kCount;++i) c.SetBase(i,solid);
    flush(); assert(output[0]==solid);
    c.SetTestRandom(true); tick(0); tick(100); assert(batches==1 && output[0]!=solid);
    const auto saved=output; const auto count=refreshes;
    tick(110); assert(output==saved && refreshes==count);
    tick(130,false); assert(output[0]==solid);
    c.SetConversation(speech); flush(); assert(output[0]==speech);
    c.SetConversationRandom(); tick(160,false); tick(260,false); assert(output[0]!=speech);
    c.SetBase(0,solid); flush(); assert(output[0]!=solid);
    c.ReleaseConversation(); tick(300,false); assert(output[0]==solid);
    tick(330); tick(430); assert(output[0]!=solid);
    c.SetSleepBlackout(true); flush(); tick(450); for(auto p:output) assert(p==off);
    c.SetSleepBlackout(false); tick(480,false); flush(); assert(output[0]==solid);
    tick(510); tick(610); assert(output[0]!=solid);
    c.CancelRandom(); flush(); tick(640); assert(output[0]==solid);
    c.SetTestRandom(true); tick(670); tick(770); assert(output[0]!=solid);
    tick(800,true,true); assert(output[0]==solid);
    tick(900,true,false); assert(output[0]==solid);
}
