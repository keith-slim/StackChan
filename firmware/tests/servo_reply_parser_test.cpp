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
    std::puts("servo_reply_parser_test: 9 cases passed");
}
