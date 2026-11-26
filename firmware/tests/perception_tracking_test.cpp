#include "stackchan/port_stage1/perception_tracking.h"
#include <cassert>
#include <cmath>
using namespace stackchan::stage1::tracking;
int main() {
    assert(WithinSkew(100, 180, 80));
    assert(!WithinSkew(100, 181, 80));
    assert(WithinSkew(5, UINT32_MAX - 4, 10));
    assert(std::abs(FilterCoordinate(0.4f, 0.45f, 0, kYawGain) - 0.4175f) < 0.00001f);
    // Stationary smoothing remains; actual travel removes the artificial lag.
    assert(std::abs(FilterCoordinate(0.2f, 0.3f, 50, -kPitchGain) - 0.3f) < 0.00001f);
    assert(FilterCoordinate(0.32f, 0.52f, 0, -kPitchGain) == 0.5f);
    assert(FilterCoordinate(0.68f, 0.48f, 0, kYawGain) == 0.5f);
    assert(FaceDelta(0.52f, 0.32f, -kPitchGain, kPitchDeadband, 110, false) == 0);
    assert(FaceDelta(0.70f, 0.30f, kYawGain, kYawDeadband, 160, false) == 0);
    assert(FaceDelta(0.25f, 0.25f, -kPitchGain, kPitchDeadband, 110, false) == 90);
    assert(FaceDelta(0.75f, 0.75f, -kPitchGain, kPitchDeadband, 110, false) == -90);
    assert(FaceDelta(1.0f, 1.0f, kYawGain, kYawDeadband, 160, false) == 160);
    assert(FaceDelta(0.2f, 0.2f, kYawGain, kYawDeadband, 160, true) == 0);
    assert(CaptureTarget(100, 90, 30, 870) == 190);
    assert(CaptureTarget(860, 90, 30, 870) == 870);
    assert(CaptureTarget(40, -90, 30, 870) == 30);
    assert(CaptureTarget(100, 0, 30, 870) == 100);
}
