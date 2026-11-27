#include "servo_reply_parser.h"
#include <cstdlib>
#include <cstdio>
#include <vector>
using ftservo::ReplyParser;
static void check(bool ok) { if (!ok) std::abort(); }
static std::vector<uint8_t> frame(uint8_t id, uint8_t status, std::vector<uint8_t> data) {
    std::vector<uint8_t> f{255,255,id,static_cast<uint8_t>(data.size()+2),status};
    f.insert(f.end(),data.begin(),data.end());
    uint8_t sum=0; for(size_t i=2;i<f.size();++i) sum+=f[i]; f.push_back(static_cast<uint8_t>(~sum));
    return f;
}
static bool feed(ReplyParser& p, const std::vector<uint8_t>& f) {
    for(auto b:f) if(p.Feed(b)==ReplyParser::Event::Match) return true;
    return false;
}
int main() {
    auto valid=frame(1,0,{12,34});
    { ReplyParser p(1,2,56,true); check(feed(p,valid)); uint8_t data[2]; p.CopyData(data); check(data[0]==12&&data[1]==34&&p.Status()==0); }
    { ReplyParser p(1,2,56,true); check(!feed(p,{1,2,3,255})); check(feed(p,valid)); }
    { ReplyParser p(1,2,56,true); check(!feed(p,frame(2,0,{12,34}))); check(feed(p,valid)); }
    { ReplyParser p(1,2,56,true); check(!feed(p,frame(1,0,{}))); check(feed(p,valid)); }
    { ReplyParser p(1,2,56,true); auto bad=valid; bad.back()^=1; check(!feed(p,bad)); check(feed(p,valid)); }
    { ReplyParser p(1,2,56,true); check(!feed(p,frame(1,2,{56,2}))); check(feed(p,valid)); }
    { ReplyParser p(2,15,56,true); std::vector<uint8_t> data(15,123); check(feed(p,frame(2,0,data))); uint8_t out[15]; p.CopyData(out); for(auto b:out) check(b==123); }
    { ReplyParser p(1,0,0,false); check(!feed(p,frame(2,0,{}))); check(feed(p,frame(1,0,{}))); }
    { ReplyParser p(1,2,56,true); check(!feed(p,{255,255,1,4,0,1})); check(feed(p,valid)); }
    // A complete embedded reply after a truncated leading frame is already
    // available at the final byte; no extra byte may be required to rescan.
    { ReplyParser p(1,15,56,true); auto reply=frame(1,0,std::vector<uint8_t>(15,12)); auto prefix=std::vector<uint8_t>{255,255,1,17,0}; prefix.insert(prefix.end(),reply.begin(),reply.end()); check(feed(p,prefix)); }
    { ReplyParser p(2,15,56,true); auto ack=frame(1,0,{}); ReplyParser::Event e=ReplyParser::Event::None; for(auto b:ack)e=p.Feed(b); check(e==ReplyParser::Event::AckSkipped); check(feed(p,frame(2,0,std::vector<uint8_t>(15,123)))); }
    { auto echo=frame(1,3,{42,0,100,0,20,0,0}); ReplyParser p(1,0,0,false,echo.data(),echo.size()); check(!feed(p,echo)); check(feed(p,frame(1,0,{}))); uint8_t out[6]{}; p.CopyFrame(out); check(out[2]==1&&out[3]==2&&out[4]==0); }
    { ReplyParser p(2,15,56,true); check(!feed(p,frame(2,2,{56,15}))); check(feed(p,frame(2,0,std::vector<uint8_t>(15,123)))); }
    { ReplyParser p(1,1,66,true); check(!feed(p,frame(1,0,{}))); check(feed(p,frame(1,0,{1}))); }
    std::puts("servo_reply_parser_test: 14 cases passed");
}
