#include "stage1_clock.h"
#include <cassert>

using namespace stackchan::stage1;

static State step(Clock& c, uint32_t ms, uint16_t minute,
                  bool standby = true, bool activity = false, uint32_t random = 0)
{
    return c.Step({ms, minute, true, standby, activity, random});
}

int main()
{
    Clock clock;
    assert(!Clock::IsDay(359, clock.GetConfig()));
    assert(Clock::IsDay(360, clock.GetConfig()));
    assert(Clock::IsDay(1379, clock.GetConfig()));
    assert(!Clock::IsDay(1380, clock.GetConfig()));
    Config invalid = clock.GetConfig();
    invalid.sleep_minute = invalid.wake_minute;
    assert(!clock.SetConfig(invalid));

    // A schedule crossing midnight keeps the original day/night semantics.
    Config cross = clock.GetConfig();
    cross.wake_minute = 1320;
    cross.sleep_minute = 360;
    assert(clock.SetConfig(cross));
    assert(Clock::IsDay(10, cross));
    assert(!Clock::IsDay(1000, cross));
    clock.Reset();
    assert(step(clock, 1, 1000).phase == Phase::NightSleep);
    auto s = step(clock, 2, 1400);
    assert(s.phase == Phase::MorningWake && s.wake_reason == WakeReason::Morning);
    assert(step(clock, 3000, 1400).phase == Phase::Awake);

    // S3 bedtime order: 10s drowsy, message, 3s readable hold, yawn, sleep.
    clock.SetConfig(Config{});
    clock.Reset();
    assert(step(clock, 1, 500).phase == Phase::Awake);
    assert(step(clock, 2, 1400, false).phase == Phase::NightPending);
    assert(step(clock, 3, 1400).phase == Phase::NightDrowsy);
    assert(step(clock, 10003, 1400).phase == Phase::NightMessage);
    assert(step(clock, 13002, 1400).phase == Phase::NightMessage);
    assert(step(clock, 13003, 1400).phase == Phase::NightYawn);
    assert(step(clock, 16503, 1400).phase == Phase::NightSleep);

    // Cancel a bedtime message on activity, instead of sleeping over speech.
    clock.Reset();
    step(clock, 1, 500);
    step(clock, 2, 1400);
    step(clock, 10002, 1400);
    assert(step(clock, 10003, 1400, false, true).phase == Phase::NightPending);

    // Touch and wake word are independent wake sources, both with a 2.8s
    // startled presentation. A night wake may return to sleep after 1-3 min.
    clock.Reset();
    assert(step(clock, 1, 1400).phase == Phase::NightSleep);
    assert(clock.RequestWake(WakeReason::Touch, 2));
    s = step(clock, 2, 1400, false);
    assert(s.phase == Phase::NightWake && s.wake_reason == WakeReason::Touch);
    assert(!clock.RequestWake(WakeReason::Word, 4));
    assert(step(clock, 2801, 1400, false).phase == Phase::NightWake);
    assert(step(clock, 2802, 1400, false, false, 1).phase == Phase::NightTemporary);
    assert(step(clock, 2803, 1400).phase == Phase::NightTemporary);
    assert(step(clock, 62802, 1400).phase == Phase::NightTemporary);
    assert(step(clock, 62803, 1400).phase == Phase::NightSleep);

    clock.Reset();
    assert(step(clock, 1, 1400).phase == Phase::NightSleep);
    assert(clock.RequestWake(WakeReason::Word, 2));
    s = step(clock, 2, 1400, false);
    assert(s.phase == Phase::NightWake && s.wake_reason == WakeReason::Word);

    // S3 morning is a sleepy wake, not a startled/touch wake.
    clock.Reset();
    assert(step(clock, 1, 1400).phase == Phase::NightSleep);
    s = step(clock, 2, 360);
    assert(s.phase == Phase::MorningWake && s.wake_reason == WakeReason::Morning);
    assert(step(clock, 2802, 360).phase == Phase::Awake);

    // Night activity holds a temporary wake period instead of instantly
    // resuming its sleep animation.
    clock.Reset();
    step(clock, 1, 1400);
    s = step(clock, 2, 1400, false);
    assert(s.phase == Phase::NightTemporary);
    assert(step(clock, 3, 1400, false).phase == Phase::NightTemporary);
    assert(step(clock, 120002, 1400).phase == Phase::NightTemporary);
    assert(step(clock, 120003, 1400).phase == Phase::NightSleep);

    // Wraparound while entering the bedtime message and shared yawn.
    clock.Reset();
    step(clock, 0xffffff00u, 500);
    assert(step(clock, 0xffffff01u, 1400).phase == Phase::NightDrowsy);
    assert(step(clock, 10000 - 255, 1400).phase == Phase::NightMessage);
    assert(step(clock, 13000 - 255, 1400).phase == Phase::NightYawn);

    // Timed nap wake, including the distinct S3 wake reason.
    Clock naps;
    Config nap{};
    nap.theatre = false;
    nap.nap_chance = 100;
    nap.nap_sleep_min_minutes = nap.nap_sleep_max_minutes = 1;
    assert(naps.SetConfig(nap));
    assert(step(naps, 0, 500).phase == Phase::Awake);
    step(naps, 1, 500);
    assert(step(naps, 600002, 500).phase == Phase::NapDrowsy);
    assert(step(naps, 626502, 500).phase == Phase::NapYawn);
    assert(step(naps, 630002, 500).phase == Phase::NapSleep);
    assert(step(naps, 689999, 500).phase == Phase::NapSleep);
    s = step(naps, 690002, 500);
    assert(s.phase == Phase::NapWake && s.wake_reason == WakeReason::Timer);
    assert(step(naps, 692802, 500).phase == Phase::Awake);

    // Wake from daytime sleep and from the yawn with a pad/screen tap.
    naps.Reset(); step(naps, 0, 500); step(naps, 1, 500);
    step(naps, 600002, 500); step(naps, 626502, 500);
    assert(naps.RequestWake(WakeReason::Touch, 626503));
    s = step(naps, 626503, 500);
    assert(s.phase == Phase::NapWake && s.wake_reason == WakeReason::Touch);

    naps.Reset(); step(naps, 0, 500); step(naps, 1, 500);
    step(naps, 600002, 500); step(naps, 626502, 500);
    step(naps, 630002, 500);
    assert(naps.RequestWake(WakeReason::Word, 630003));
    s = step(naps, 630003, 500, false);
    assert(s.phase == Phase::NapWake && s.wake_reason == WakeReason::Word);

    // The S3 voice service may need several seconds to generate and play a
    // greeting. Sleep/wake animation must wait for it, not cut it at 2.8/3s.
    Clock voiced;
    assert(step(voiced, 1, 500).phase == Phase::Awake);
    assert(step(voiced, 2, 1400, false).phase == Phase::NightPending);
    assert(step(voiced, 3, 1400).phase == Phase::NightDrowsy);
    assert(step(voiced, 10003, 1400).phase == Phase::NightMessage);
    assert(voiced.Step({17003, 1400, true, true, false, 0, true}).phase == Phase::NightMessage);
    assert(voiced.Step({17004, 1400, true, true, false, 0, false}).phase == Phase::NightYawn);

    voiced.Reset();
    assert(step(voiced, 1, 1400).phase == Phase::NightSleep);
    assert(voiced.RequestWake(WakeReason::Touch, 2));
    assert(voiced.Step({10002, 1400, true, true, false, 0, true}).phase == Phase::NightWake);
    assert(voiced.Step({10003, 1400, true, true, false, 0, false}).phase == Phase::NightTemporary);

    voiced.Reset();
    assert(step(voiced, 1, 1400).phase == Phase::NightSleep);
    assert(voiced.Step({2, 360, true, true, false, 0, false}).phase == Phase::MorningWake);
    assert(voiced.Step({9000, 360, true, true, false, 0, true}).phase == Phase::MorningWake);
    assert(voiced.Step({9001, 360, true, true, false, 0, false}).phase == Phase::Awake);

    naps.Reset(); step(naps, 0, 500); step(naps, 1, 500);
    step(naps, 600002, 500); step(naps, 626502, 500);
    assert(naps.RequestWake(WakeReason::Touch, 626503));
    assert(naps.Step({640000, 500, true, true, false, 0, true}).phase == Phase::NapWake);
    assert(naps.Step({640001, 500, true, true, false, 0, false}).phase == Phase::Awake);

    // An explicit spoken nap is allowed even if random naps are disabled.
    Clock manual;
    Config manual_cfg{};
    manual_cfg.theatre = false;
    manual_cfg.naps = false;
    manual_cfg.nap_sleep_min_minutes = 1;
    manual_cfg.nap_sleep_max_minutes = 1;
    assert(manual.SetConfig(manual_cfg));
    assert(step(manual, 1, 500).phase == Phase::Awake);
    assert(!manual.RequestSleep(2, 500, false));
    assert(manual.RequestSleep(2, 500, true));
    assert(step(manual, 2, 500).phase == Phase::NapYawn);
    assert(step(manual, 3502, 500).phase == Phase::NapSleep);
    assert(step(manual, 63502, 500).phase == Phase::NapWake);
    assert(step(manual, 66302, 500).phase == Phase::Awake);

    // A manual night request reuses the existing sleep state instead of
    // changing the bedtime schedule or the quiet-mode suppression flag.
    manual.Reset();
    assert(step(manual, 1, 1400, false).phase == Phase::NightTemporary);
    assert(manual.RequestSleep(2, 1400, true));
    assert(step(manual, 2, 1400).phase == Phase::NightSleep);
    assert(manual.RequestWake(WakeReason::Touch, 3));
    assert(step(manual, 3, 1400, false).phase == Phase::NightWake);

    // No accidental startled wake while already awake.
    assert(!naps.RequestWake(WakeReason::Touch, 700000));
}
