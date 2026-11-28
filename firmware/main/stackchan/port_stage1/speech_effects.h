#pragma once
// Shared, allocation-free conversation expression presets. Servo units are
// tenths of a degree, with negative pitch looking down.
#include "../avatar/avatar/elements/emotion.h"
#include <cstdint>

namespace stackchan::stage1 {
// Shared servo-test targets: keep the original Web test values unchanged.
struct GestureTarget { int yaw, pitch, speed; };
constexpr GestureTarget kTestNod{0,-200,285};
constexpr GestureTarget kTestShakeLeft{-125,0,320};
constexpr GestureTarget kTestShakeRight{125,0,320};
constexpr GestureTarget kTestShakeHome{0,0,300};
constexpr uint32_t kTestShakeStepMs = 420;
constexpr GestureTarget TestShakeTarget(unsigned step) {
    return step >= 4 ? kTestShakeHome :
           (step & 1u) ? kTestShakeRight : kTestShakeLeft;
}
constexpr GestureTarget kTestTilt{120,0,150};
constexpr GestureTarget kTestHappy{-90,55,180};
constexpr GestureTarget kTestHappySecond{90,55,170};
constexpr GestureTarget kTestSurprise{0,200,510};
constexpr GestureTarget kTestShy{110,-30,140};
struct SpeechEffectPreset {
    uint8_t red, green, blue;
    int yaw, pitch, speed;
    unsigned nods;
    uint32_t step_ms = 420;
    bool happy_sway = false;
    bool shake = false;
};

constexpr SpeechEffectPreset SpeechEffectFor(avatar::Emotion emotion) {
    using avatar::Emotion;
    switch (emotion) {
    case Emotion::Happy: return {110,30,65, kTestHappy.yaw,kTestHappy.pitch,kTestHappy.speed,0,400,true};
    case Emotion::Fun:   return {100,80,40, 0,55,285,2,420};
    case Emotion::Surprise: return {100,100,100,kTestSurprise.yaw,kTestSurprise.pitch,kTestSurprise.speed,1,800};
    case Emotion::Angry: return {100,0,0,kTestSurprise.yaw,kTestSurprise.pitch,kTestSurprise.speed,1,800};
    case Emotion::Sad:   return {20,32,100,kTestShakeLeft.yaw,kTestShakeLeft.pitch,kTestShakeLeft.speed,0,kTestShakeStepMs,false,true};
    case Emotion::Doubt: return {75,65,35,kTestTilt.yaw,kTestTilt.pitch,kTestTilt.speed,0};
    case Emotion::Sleepy:return {45,45,65, 0,-120,120,0};
    default:             return {0,0,50, 0,0,0,0};
    }
}

constexpr uint32_t kSpeechNodStepMs = 420;
// A late update skips expired steps instead of replaying a burst of motion.
constexpr unsigned SpeechNodStep(unsigned nods, uint32_t elapsed_ms, uint32_t step_ms = kSpeechNodStepMs) {
    if (!nods) return 0;
    const unsigned step = elapsed_ms / step_ms;
    const unsigned last = nods * 2 - 1;
    return step < last ? step : last;
}
constexpr bool SpeechNodAtOrigin(unsigned step) { return (step & 1u) != 0; }
} // namespace stackchan::stage1
