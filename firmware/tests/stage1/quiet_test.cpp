#include "../../main/stackchan/port_stage1/quiet_schedule.h"
#include <cassert>
#include <cstdlib>
using namespace stackchan::stage1::quiet_mode;
static int64_t at(int day,int hour,int minute) {
    std::tm t{};t.tm_year=126;t.tm_mon=10;t.tm_mday=day;t.tm_hour=hour;t.tm_min=minute;
    return std::mktime(&t);
}
int main() {
    setenv("TZ","JST-9",1);tzset();
    auto noon=at(20,12,0);
    Settings s;Request r;r.action=Action::Schedule;r.start=720;r.end=780;
    assert(Modify(s,r,noon)&&Evaluate(s,noon).active);
    assert(!Evaluate(s,at(20,13,0)).active);
    r.action=Action::Cancel;assert(Modify(s,r,noon)&&!Evaluate(s,noon).active&&Evaluate(s,at(21,12,0)).active);
    r.action=Action::Schedule;r.daily=false;r.start=1380;r.end=420;
    assert(Modify(s,r,at(20,22,0))&&Evaluate(s,at(21,1,0)).active&&!Evaluate(s,at(22,1,0)).active);
    r.action=Action::Minutes;r.minutes=60;assert(Modify(s,r,at(21,6,30))&&Evaluate(s,at(21,6,30)).until==at(21,7,30));
    r.action=Action::Until;r.end=60;assert(Modify(s,r,at(21,2,0))&&s.temporary_until==at(22,1,0));
    assert(!Evaluate(s,0).active&&!Evaluate(s,0).clock_ready);
    r.action=Action::Minutes;r.minutes=0;assert(!Modify(s,r,noon));
    r.action=Action::Schedule;r.start=r.end;assert(!Modify(s,r,noon));
}
