/* Bounded, opt-in compositor timing; no per-frame heap storage. */
#pragma once
#include "TranslationPolicy.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
namespace fuxi {
using FrameClock=std::chrono::steady_clock;
inline uint64_t frameNanos(FrameClock::time_point start) {
 return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(FrameClock::now()-start).count());
}
inline unsigned frameCPUFromRegister(uint64_t value){return unsigned(value&0xfff);}
inline unsigned frameCPU() {
#if defined(__APPLE__) && defined(__aarch64__) && !defined(FUXI_MTL_HOST_TEST)
 // XNU 8796 machine_machdep.h and libsyscall/os/tsd.h: TPIDR_EL0 low 12 bits.
 uint64_t value;__asm__ __volatile__("mrs %0, TPIDR_EL0":"=r"(value));return frameCPUFromRegister(value);
#else
 return 4096;
#endif
}
struct FrameCounts {uint64_t pipelines=0,descriptors=0,allocations=0;};
// Count successful Vulkan pipeline/set/memory creations on the calling thread.
inline thread_local FrameCounts frameCounts;
struct FrameSample {
 uint64_t wall=0,cpuBeforeCommit=0,shadowCopy=0,leaseLock=0,backendSubmit=0,gpuWait=0,gpuExec=0;
 uint64_t draws=0,passes=0,gpuSamples=0;FrameCounts counts{};unsigned cpu=4096;bool encodeCountsUnknown=false;
};
inline thread_local FrameSample *currentFrame=nullptr;
struct FrameTraceScope {
 FrameSample *previous;
 explicit FrameTraceScope(FrameSample *sample):previous(currentFrame){currentFrame=sample;}
 ~FrameTraceScope(){currentFrame=previous;}
};
// Only BackboardGate accesses this, under its existing reporter/completion lock.
class FrameTraceWindow {
 static constexpr size_t capacity=1024;
 std::array<FrameSample,capacity> samples{};
 std::array<uint64_t,capacity> scratch{};
 uint64_t count=0,wallMax=0,draws=0,passes=0,encodeUnknown=0,gpuUnavailable=0;
 FrameCounts counts{};std::array<uint64_t,9> cpus{};
public:
 void add(const FrameSample &s) {
  samples[count%capacity]=s;++count;wallMax=std::max(wallMax,s.wall);
  draws+=s.draws;passes+=s.passes;counts.pipelines+=s.counts.pipelines;counts.descriptors+=s.counts.descriptors;counts.allocations+=s.counts.allocations;
  gpuUnavailable+=!s.gpuSamples;++cpus[s.cpu<8?s.cpu:8];encodeUnknown+=s.encodeCountsUnknown;
 }
 uint64_t size() const{return count;}
 uint64_t percentile(uint64_t FrameSample::*field,unsigned percent) {
  size_t n=size_t(std::min<uint64_t>(count,capacity));if(!n)return 0;
  for(size_t i=0;i<n;++i)scratch[i]=samples[i].*field;
  std::sort(scratch.begin(),scratch.begin()+n);return scratch[(n*percent+99)/100-1];
 }
 void report(FILE *out=stderr) {
  auto p=[&](uint64_t FrameSample::*field,unsigned percent){return percentile(field,percent)/1e6;};
  double n=count?double(count):1;
  flockfile(out); // Keep the histogram on the same line as its percentiles.
  fprintf(out,"FUXI MTL: frame trace n=%llu samples=%llu wall=%.3f/%.3f/%.3f ms cpu_before_commit=%.3f/%.3f shadow_copy=%.3f/%.3f lease_lock=%.3f/%.3f backend_submit=%.3f/%.3f gpu_wait=%.3f/%.3f gpu_exec=%.3f/%.3f draws/frame=%.2f render_passes/frame=%.2f pipelines/frame=%.2f descriptors/frame=%.2f allocations/frame=%.2f cpu_hist=",
   (unsigned long long)count,(unsigned long long)std::min<uint64_t>(count,capacity),
   p(&FrameSample::wall,50),p(&FrameSample::wall,90),wallMax/1e6,
   p(&FrameSample::cpuBeforeCommit,50),p(&FrameSample::cpuBeforeCommit,90),
   p(&FrameSample::shadowCopy,50),p(&FrameSample::shadowCopy,90),
   p(&FrameSample::leaseLock,50),p(&FrameSample::leaseLock,90),
   p(&FrameSample::backendSubmit,50),p(&FrameSample::backendSubmit,90),
   p(&FrameSample::gpuWait,50),p(&FrameSample::gpuWait,90),
   p(&FrameSample::gpuExec,50),p(&FrameSample::gpuExec,90),draws/n,passes/n,counts.pipelines/n,counts.descriptors/n,counts.allocations/n);
  for(unsigned i=0;i<8;++i)fprintf(out,"%s%u:%llu",i?",":"",i,(unsigned long long)cpus[i]);
  fprintf(out,",unknown:%llu encode_counts_unknown=%llu gpu_exec_unavailable=%llu\n",(unsigned long long)cpus[8],(unsigned long long)encodeUnknown,(unsigned long long)gpuUnavailable);
  funlockfile(out);
  count=wallMax=draws=passes=encodeUnknown=gpuUnavailable=0;counts={};cpus.fill(0);
 }
};
}
