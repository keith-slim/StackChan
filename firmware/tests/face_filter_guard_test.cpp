#include "face_filter_guard.h"
#include <cstdlib>
#include <cstdio>
#include <limits>
using namespace stackchan::stage1;
static void check(bool ok) { if (!ok) std::abort(); }
static bool near(float actual, float expected) { return std::abs(actual - expected) < 0.0001f; }
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

    check(near(NormalizedFaceHeight(30, 130, 240), 100.0f / 240.0f));
    check(near(NormalizedFaceHeight(-10, 50, 200), 0.25f));
    check(NormalizedFaceHeight(60, 50, 240) == 0.0f);
    check(NormalizedFaceHeight(20, 50, 0) == 0.0f);
    check(NormalizedFaceHeight(20, std::numeric_limits<float>::infinity(), 240) == 0.0f);

    FacePitchHold far_face, near_face;
    check(far_face.Update(0.055f, 0.055f, 0.15f) == 0.0f);
    check(near(far_face.ResumeBand(), 0.035f));
    check(far_face.Update(0.055f, 0.055f, 0.15f) == 0.055f);
    check(near_face.Update(0.055f, 0.055f, 100.0f / 240.0f) == 0.0f);
    check(near(near_face.ResumeBand(), 0.075f));
    check(near_face.Update(0.055f, 0.055f) == 0.0f);
    check(near_face.Update(0.092f, 0.088f) == 0.0f);
    check(near_face.Update(0.092f, 0.088f) == 0.088f);
    check(near_face.Update(0.042f, 0.042f) == 0.0f);

    // Losing one detection clears motion confirmation while retaining the face scale.
    near_face.ResetMotion();
    check(near(near_face.ResumeBand(), 0.075f));
    near_face.Update(0.0f, 0.0f, 0.20f);
    check(near(near_face.ResumeBand(), 0.075f * 0.8f + 0.20f * 0.18f * 0.2f));
    near_face.Update(0.0f, 0.0f, std::numeric_limits<float>::quiet_NaN());
    check(near(near_face.ResumeBand(), 0.0672f));
    near_face.Reset();
    check(near(near_face.ResumeBand(), 0.060f));
    near_face.Update(0.0f, 0.0f, 0.80f);
    check(near(near_face.ResumeBand(), 0.10f));
    std::puts("face_filter_guard_test: passed");
}
