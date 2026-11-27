#include "stackchan/config/server_boot_policy.h"
#include <cstdlib>
#include <cstdio>
using namespace stackchan::config::server_boot;
static void check(bool readable, bool ws, bool canonical, bool mqtt, bool legacy,
                  int version, Protocol expected, bool seed_url = false, bool seed_version = false) {
    auto p = PlanBoot(readable, ws, canonical, mqtt, legacy, version);
    if (p.protocol != expected || p.seed_url != seed_url || p.seed_version != seed_version) std::abort();
}
int main() {
    check(true,true,true,false,false,3,Protocol::Websocket);
    check(true,true,true,false,true,1,Protocol::Websocket);
    check(true,true,true,false,true,2,Protocol::Websocket);
    check(true,true,true,false,false,0,Protocol::Websocket,false,true);
    check(true,false,false,false,true,0,Protocol::Websocket,true,true);
    check(true,false,false,false,true,2,Protocol::Websocket,true,false);
    check(true,false,false,true,true,0,Protocol::Mqtt);
    check(true,true,true,true,true,3,Protocol::Legacy);
    check(true,true,false,false,true,3,Protocol::Legacy);
    check(true,false,false,false,false,0,Protocol::Legacy);
    check(true,true,true,false,true,4,Protocol::Legacy);
    check(true,true,true,false,true,-1,Protocol::Legacy);
    check(false,true,true,false,true,3,Protocol::Legacy);
    std::puts("server_boot_policy_test: 13 cases passed");
}
