#include "stackchan/config/low_battery.h"
#include <cassert>
using namespace stackchan::low_battery;
int main() {
    Detector d;
    Sample low{10, true, true, false, false, true};
    Sample usb = low; usb.external = true;
    Sample invalid = low; invalid.valid = false;
    Sample high = low; high.level = 11;
    Sample absent = low; absent.present = false;
    assert(!d.Observe(high));
    assert(!d.Observe(low)); assert(!d.Observe(low));
    assert(!d.Observe(invalid));
    assert(!d.Observe(low)); assert(!d.Observe(low)); assert(d.Observe(low));
    assert(d.Observe(high)); assert(d.Observe(invalid));
    assert(!CanPowerOff(usb) && !CanPowerOff(invalid) && !CanPowerOff(absent));
    assert(!d.Observe(usb));
    for (int i = 0; i < 5; ++i) assert(!d.Observe(usb));
    assert(!d.Observe(low)); assert(!d.Observe(low)); assert(d.Observe(low));
    assert(!d.Observe(absent));
    Sequence s; s.Begin(100);
    assert(!s.Tick(15099,false,true));
    s.Queued(200); assert(!s.Tick(500,true,false));
    assert(!s.Tick(1000,false,false)); assert(s.phase == Phase::Speech);
    assert(!s.Tick(2000,false,true)); assert(s.phase == Phase::Grace);
    assert(!s.Tick(31999,false,true)); assert(s.Tick(32000,false,true));
    s.Begin(0); assert(!s.Tick(15000,false,true));
    assert(s.phase == Phase::Grace); assert(s.Tick(45000,false,true));
    s.Begin(0); s.Queued(100); assert(!s.Tick(45100,true,false));
    assert(s.phase == Phase::Grace); assert(s.Tick(75100,true,false));
    s.Begin(0xfffffff0U); s.Queued(10); assert(!s.Tick(20,false,true));
    assert(s.Tick(30020,false,true));
    Publish(low,true); assert(Active());
    const auto old_epoch = Epoch();
    MarkReady(old_epoch); assert(Ready());
    Publish(usb,false); assert(!Ready() && !Active());
    Publish(low,true); assert(Active() && !Ready());
    MarkReady(old_epoch); assert(!Ready());
    MarkReady(Epoch()); assert(Ready());
    Publish(usb,false); assert(!Ready() && !Active());
}
