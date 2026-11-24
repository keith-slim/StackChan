#include <stackchan/port_stage1/speech_effects.h>
#include <cassert>
#include <initializer_list>
using stackchan::avatar::Emotion;
using namespace stackchan::stage1;

int main() {
    static_assert(SpeechEffectFor(Emotion::Neutral).speed == 0);
    static_assert(SpeechEffectFor(Emotion::Happy).happy_sway);
    static_assert(SpeechEffectFor(Emotion::Surprise).pitch == kTestSurprise.pitch);
    static_assert(SpeechEffectFor(Emotion::Fun).nods == 2);
    static_assert(SpeechEffectFor(Emotion::Angry).red == 100);
    static_assert(SpeechEffectFor(Emotion::Sad).shake);
    static_assert(SpeechEffectFor(Emotion::Angry).pitch == kTestSurprise.pitch);
    static_assert(SpeechEffectFor(Emotion::Fun).pitch == 55);
    static_assert(TestShakeTarget(0).yaw == -125 && TestShakeTarget(1).yaw == 125);
    static_assert(TestShakeTarget(2).yaw == -125 && TestShakeTarget(3).yaw == 125);
    static_assert(TestShakeTarget(4).yaw == 0);
    static_assert(SpeechEffectFor(Emotion::Doubt).yaw != 0);
    static_assert(SpeechEffectFor(Emotion::Sleepy).pitch < 0);
    for (const auto emotion : {Emotion::Neutral, Emotion::Happy, Emotion::Fun,
                               Emotion::Angry, Emotion::Sad, Emotion::Doubt, Emotion::Sleepy, Emotion::Surprise}) {
        const auto p = SpeechEffectFor(emotion);
        assert(p.red <= 168 && p.green <= 168 && p.blue <= 168);
        assert(p.yaw >= -125 && p.yaw <= 125 && p.pitch >= -200 && p.pitch <= 200);
    }
    static_assert(SpeechNodStep(1, 0) == 0);
    static_assert(SpeechNodStep(1, 420) == 1);
    static_assert(SpeechNodStep(1, 5000) == 1);
    static_assert(SpeechNodStep(2, 419) == 0);
    static_assert(SpeechNodStep(2, 420) == 1);
    static_assert(SpeechNodStep(2, 840) == 2);
    static_assert(SpeechNodStep(2, 1260) == 3);
    static_assert(SpeechNodStep(2, 5000) == 3);
    static_assert(!SpeechNodAtOrigin(0) && SpeechNodAtOrigin(1));
    static_assert(!SpeechNodAtOrigin(2) && SpeechNodAtOrigin(3));
}
