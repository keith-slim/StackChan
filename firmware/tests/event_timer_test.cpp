#include "stackchan/event_timer.h"
#include <cassert>
using namespace stackchan::event_timer;
int main() {
    uint32_t deadline = 100;
    int draws = 0;
    const auto delay = [&] { ++draws; return 50U; };
    assert(!DiscardDue(deadline, 99, delay) && deadline == 100 && draws == 0);
    assert(DiscardDue(deadline, 100, delay) && deadline == 150 && draws == 1);
    assert(!DiscardDue(deadline, 101, delay) && draws == 1);
    assert(DiscardDue(deadline, 175, delay) && deadline == 225 && draws == 2);
    deadline = 0;
    assert(!DiscardDue(deadline, 100, delay) && draws == 2);
    deadline = Next(0xfffffff0U, 32);
    assert(deadline == 16 && !Due(deadline, 0xffffffffU) && Due(deadline, 16));
    assert(Next(0xfffffff0U, 16) == 1);
    assert(DiscardDue(deadline, 16, delay) && deadline == 66);
}
