/* CPU timeline for the synchronous NativeMetal-style event adapter.
 * Process-local only: no kernel/shared handle or cross-process ordering. */
#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
namespace fuxi {
class Timeline {
 std::mutex mutex;std::condition_variable changed;uint64_t current=0;
public:
 uint64_t value(){std::lock_guard<std::mutex> lock(mutex);return current;}
 void signal(uint64_t value){std::lock_guard<std::mutex> lock(mutex);if(value>current){current=value;changed.notify_all();}}
 bool wait(uint64_t value,uint64_t milliseconds){
  std::unique_lock<std::mutex> lock(mutex);
  // Our command path has the same two-second ceiling as Backend fences.
  milliseconds=milliseconds>2000?2000:milliseconds;
  return changed.wait_for(lock,std::chrono::milliseconds(milliseconds),[&]{return current>=value;});
 }
};
}
