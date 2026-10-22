#include "Survey.hpp"
#include <cassert>
#include <string>
int main(){
 fuxi::Survey disabled;assert(!disabled.record("ignored",0) && !disabled.finish(100));
 fuxi::Survey timer;timer.enable(true);assert(!timer.finish(500));
 assert(timer.record("same",100));for(unsigned i=0;i<1000;++i)assert(!timer.record("same",100+i*.001));
 assert(timer.distinct()==1 && !timer.finish(119.999));assert(timer.finish(120));assert(!timer.finish(500));
 assert(!timer.record("after exit",500));
 fuxi::Survey cap;cap.enable(true);for(unsigned i=0;i<64;++i){assert(cap.record(std::to_string(i).c_str(),10));assert(cap.finish(10)==(i==63));}
 assert(cap.distinct()==64 && !cap.record("overflow",10) && !cap.finish(100));
 puts("PASS: survey opt-in, exact dedupe, 64 distinct cap, first-refusal 20s deadline, once-only exit");
}
