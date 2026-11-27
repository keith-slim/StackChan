#include "face_filter_guard.h"
#include <cstdlib>
#include <cstdio>
using namespace stackchan::stage1;
static void check(bool ok) { if (!ok) std::abort(); }
int main() {
    check(GuardedFaceError(.037f,-.065f,.025f)==0);
    check(GuardedFaceError(.049f,-.050f,.035f)==0);
    check(GuardedFaceError(.011f,.043f,.025f)==0);
    check(GuardedFaceError(-.2f,-.1f,.025f)==-.1f);
    check(GuardedFaceError(.2f,.1f,.025f)==.1f);
    check(!FaceCenterEligible(0,.116f,0,.048f));
    check(FaceCenterEligible(.02f,-.03f,.02f,-.03f));
    check(FaceCenterLost(0,.116f,0,.048f));
    check(!FaceCenterLost(.05f,.06f,.03f,.04f));
    std::puts("face_filter_guard_test: passed");
}
