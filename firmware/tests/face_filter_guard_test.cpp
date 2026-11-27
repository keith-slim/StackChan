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

    FacePitchHold pitch;
    // Small vertical errors and isolated same-side spikes keep the current pose.
    check(pitch.Update(0.049f, 0.048f) == 0.0f);
    check(pitch.Update(-0.045f, -0.041f) == 0.0f);
    check(pitch.Update(0.080f, 0.040f) == 0.0f);
    check(pitch.Update(0.071f, 0.068f) == 0.0f);
    check(pitch.Update(0.047f, 0.056f) == 0.0f);

    // Moderate displacement resumes after consecutive frames in one direction.
    check(pitch.Update(0.080f, 0.065f) == 0.0f);
    check(pitch.Update(-0.081f, -0.070f) == 0.0f);
    check(pitch.Update(-0.082f, -0.073f) == -0.073f);
    check(pitch.Update(-0.045f, -0.053f) == -0.053f);
    check(pitch.Update(-0.028f, -0.045f) == 0.0f);
    check(pitch.Update(-0.045f, -0.042f) == 0.0f);

    // A clearly displaced face can start immediately. A reversal is confirmed first.
    check(pitch.Update(0.150f, 0.110f) == 0.110f);
    check(pitch.Update(-0.140f, -0.120f) == 0.0f);
    check(pitch.Update(-0.140f, -0.120f) == -0.120f);
    check(pitch.Update(0.070f, -0.070f) == 0.0f);

    check(pitch.Update(0.080f, 0.065f) == 0.0f);
    pitch.Reset(); // Interrupted by missing face or invalid frame.
    check(pitch.Update(0.081f, 0.074f) == 0.0f);
    check(pitch.Update(0.081f, 0.074f) == 0.074f);
    std::puts("face_filter_guard_test: passed");
}
