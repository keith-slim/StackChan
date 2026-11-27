#include "stackchan/config/charge_protection.h"
#include <cassert>
using namespace stackchan::charge;
int main() {
    constexpr Settings defaults{};
    static_assert(defaults.enabled && defaults.low == 30 && defaults.high == 80);
    static_assert(ValidPacked(Pack(defaults)));
    static_assert(!ValidPacked(-1) && !ValidPacked(Pack(defaults) | (1 << 20)));
    assert(!Valid({true, 80, 80}));
    assert(!Valid({true, 9, 80}));
    assert(!Valid({true, 30, 101}));
    assert(!Valid({true, 78, 80}));
    assert(!Target(defaults, true, true, true, true, 80));
    assert(!Target(defaults, true, true, true, true, 100));
    assert(Target(defaults, false, true, true, true, 30));
    assert(Target(defaults, false, true, true, true, 0));
    for (int level = 31; level < 80; ++level) {
        assert(Target(defaults, true, true, true, true, level));
        assert(!Target(defaults, false, true, true, true, level));
    }
    assert(Target(defaults, true, false, true, true, 99));
    assert(!Target(defaults, false, false, true, true, 1));
    assert(Target(defaults, true, true, false, true, 99));
    assert(Target(defaults, true, true, true, false, 99));
    assert(!Target(defaults, false, true, true, false, 1));
    assert(Target({false,30,80}, false, true, true, true, 90));
    assert(!Target({false,30,80}, false, false, true, true, 90));
    assert(!Target(defaults, false, true, true, true, -1));
}
