#include "Timeline.hpp"
#include <cassert>
#include <thread>
#include <cstdio>
int main(){
 fuxi::Timeline event;assert(!event.value() && event.wait(0,0) && !event.wait(1,0));
 std::thread producer([&]{event.signal(7);});assert(event.wait(7,2000));producer.join();
 event.signal(3);assert(event.value()==7);event.signal(UINT64_MAX);assert(event.wait(UINT64_MAX,0));
 puts("PASS: process-local monotonic timeline, CPU wait/signal and zero timeout");
}
