#include "perception_mailbox.h"
#include <cstdlib>
#include <cstdio>
using namespace stackchan::stage1;
static void check(bool ok) { if (!ok) std::abort(); }
int main() {
    PerceptionMailbox m;
    PerceptionLook r;
    auto g = m.Generation();
    check(m.Take(100,false,r)==PerceptionMailbox::Result::Empty);
    check(m.Publish({10,20,true,100,g},101));
    check(m.Take(101,false,r)==PerceptionMailbox::Result::Waiting);
    check(m.Publish({-30,-40,false,102,g},103));
    check(!m.Publish({99,99,true,101,g},104));
    check(m.Take(104,true,r)==PerceptionMailbox::Result::Ready && r.yaw==-30 && r.pitch==-40 && !r.face);
    check(!m.Pending());
    check(m.Publish({1,2,true,200,g},201));
    m.Invalidate();
    check(!m.Pending() && !m.Publish({3,4,true,202,g},203));
    g=m.Generation();
    check(!m.Publish({1,2,true,100,g},851));
    check(m.Publish({1,2,true,300,g},300));
    check(m.Take(1051,true,r)==PerceptionMailbox::Result::Expired);
    check(m.Publish({1,2,false,0xfffffff0u,g},5));
    check(m.Take(10,true,r)==PerceptionMailbox::Result::Ready);
    check(m.Publish({1,2,true,100,g},100));
    check(m.Take(99,true,r)==PerceptionMailbox::Result::Waiting);
    check(m.Take(101,true,r)==PerceptionMailbox::Result::Ready);
    std::puts("perception_mailbox_test: passed");
}
