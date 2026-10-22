/* Bounded diagnostic collection. This never grants admission or clears a fault. */
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <memory>
#include <cstdio>
namespace fuxi {
class Survey {
public:
 static constexpr size_t messageBytes=32768;
private:
 std::mutex mutex;bool enabled=false,exited=false;double first=-1;unsigned count=0;uint64_t total=0;
 using Messages=std::array<std::array<char,messageBytes>,64>;
 std::unique_ptr<Messages> messages;
public:
 void enable(bool value){std::lock_guard<std::mutex> guard(mutex);if(value && !messages)messages=std::make_unique<Messages>();enabled=value;}
 bool active(){std::lock_guard<std::mutex> guard(mutex);return enabled;}
 bool record(const char *message,double now,bool log=false){
  std::lock_guard<std::mutex> guard(mutex);if(!enabled || exited)return false;
  if(first<0)first=now;++total;
  for(unsigned i=0;i<count;++i)if(!strcmp((*messages)[i].data(),message))return false;
  if(count==64)return false;
  snprintf((*messages)[count++].data(),messageBytes,"%s",message);if(log)fputs(message,stderr);return true;
 }
 bool finish(double now){
  std::lock_guard<std::mutex> guard(mutex);
  if(!enabled || exited || first<0 || (count<64 && now-first<20))return false;
  exited=true;fprintf(stderr,"FUXI MTL: backboardd survey summary distinct=%u refusals=%llu seconds=%.3f reason=%s next-respawn=CPU\n",count,(unsigned long long)total,now-first,count==64?"cap":"timer");return true;
 }
 unsigned distinct(){std::lock_guard<std::mutex> guard(mutex);return count;}
};
}
