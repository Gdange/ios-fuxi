/* Original host controls for bounded frame statistics and thread attribution. */
#define FUXI_MTL_HOST_TEST
#include "FrameTrace.hpp"
#include <cassert>
#include <cstring>
#include <thread>
int main(){
 using namespace fuxi;
 assert(frameCPUFromRegister(0xabcdef000ULL|6)==6);
 assert(frameCPUFromRegister(0xabc000ULL|0xfff)==4095);
 assert(frameCPU()==4096); // host has no Darwin register convention
 FrameTraceWindow window;assert(window.percentile(&FrameSample::wall,50)==0);
 for(unsigned i=1;i<=100;++i){FrameSample s;s.wall=i*1000000ULL;s.cpu=i%7;s.draws=2;s.passes=1;s.counts={1,2,3};window.add(s);}
 assert(window.size()==100 && window.percentile(&FrameSample::wall,50)==50000000 && window.percentile(&FrameSample::wall,90)==90000000);
 FILE *out=tmpfile();assert(out);window.report(out);rewind(out);char line[2048];assert(fgets(line,sizeof(line),out));
 assert(strstr(line,"frame trace n=100 samples=100 wall=50.000/90.000/100.000 ms"));
 assert(strstr(line,"draws/frame=2.00 render_passes/frame=1.00 pipelines/frame=1.00 descriptors/frame=2.00 allocations/frame=3.00"));
 assert(!window.size());fclose(out);
 // Overflow retains the newest 1024 samples; window max includes evicted data.
 FrameSample s;s.wall=9999999999;window.add(s);
 for(unsigned i=0;i<2048;++i){s.wall=1000000;window.add(s);}
 assert(window.size()==2049 && window.percentile(&FrameSample::wall,90)==1000000);
 out=tmpfile();assert(out);window.report(out);rewind(out);assert(fgets(line,sizeof(line),out));fclose(out);
 assert(strstr(line,"n=2049 samples=1024 wall=1.000/1.000/10000.000"));
 FrameSample first,second;assert(!currentFrame);
 {FrameTraceScope a(&first);assert(currentFrame==&first);
  {FrameTraceScope b(&second);assert(currentFrame==&second);}assert(currentFrame==&first);
  std::thread worker([]{assert(!currentFrame && frameCounts.allocations==0);frameCounts.allocations=3;});worker.join();
  assert(!frameCounts.allocations);
 }assert(!currentFrame);
 puts("PASS: bounded frame percentiles/reset/overflow/max, log fields, CPU mask and thread-local attribution");
}
