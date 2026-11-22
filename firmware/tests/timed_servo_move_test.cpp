#include "stackchan/motion/timed_servo_move.h"
#include <cassert>

using stackchan::motion::TimedServoMove;
int main() {
    TimedServoMove move;
    move.Start(600, 0, 10000, 100);
    assert(move.Active());
    assert(move.Sample(100) == 600);
    assert(move.Sample(200) == 594); // visible early travel, no spring restart
    assert(move.Sample(5100) == 300);
    assert(move.Sample(10100) == 0 && !move.Active());
    move.Start(-350, 30, 3000, 0xffffff00u);
    assert(move.Sample(0xffffff00u + 1500u) == -160);
    assert(move.Sample(0xffffff00u + 3000u) == 30 && !move.Active());
    move.Start(30, 30, 1000, 0);
    assert(!move.Active());
    move.Start(100, 0, 1000, 0);
    move.Cancel();
    assert(!move.Active());
    move.Start(100, 0, 0, 10);
    assert(move.Sample(11) == 0 && !move.Active());
}
