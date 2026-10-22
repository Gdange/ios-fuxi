/* Original MIT controls against the external patched Rust cdylib, lavapipe.
 * No shaders/Apple fixtures. CPU-coherent x86 cache flush is a test-only stub. */
#include "NativeMetalObjects.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#ifdef __linux__
extern "C" void sys_dcache_flush(void *,size_t) {}
#endif
static std::atomic<unsigned> scheduled{0},completed{0};
int main(int argc,char **argv){
 assert(argc==3);size_t bytes=1;
 nmb_fixture_air(&bytes);assert(!bytes);nmb_function_constant_air(&bytes);assert(!bytes);
 assert(!nmb_device_open(nullptr,argv[2]) && strlen(nmb_last_error()));
 NmbDevice *device=nmb_device_open(argv[1],argv[2]);
 if(!device){fprintf(stderr,"FAIL: %s\n",nmb_last_error());return 1;}
 auto *queue=nmb_queue_new(device);assert(queue);
 auto *src=nmb_buffer_new(device,16384,0,nullptr),*dst=nmb_buffer_new(device,16384,0,nullptr);assert(src && dst);
 memset(nmb_buffer_contents(src),0x21,16384);memset(nmb_buffer_contents(dst),0x73,16384);
 auto *command=nmb_command_buffer_new(queue);assert(command);
 auto *encoder=nmb_blit_encoder_new(command);assert(encoder);
 assert(nmb_blit_fill_buffer(encoder,src,4,16380,0xa6));
 assert(nmb_blit_copy_buffer(encoder,src,4,dst,8,16368));assert(nmb_encoder_end(encoder));
 assert(nmb_command_buffer_on_scheduled(command,[](void *){++scheduled;},nullptr));
 assert(nmb_command_buffer_on_completed(command,[](void *){++completed;},nullptr));
 assert(nmb_command_buffer_commit(command));assert(nmb_command_buffer_wait_scheduled(command));
 assert(nmb_command_buffer_wait(command,2000)==0 && nmb_command_buffer_status(command)==4);
 assert(scheduled==1 && completed==1);
 auto *p=(unsigned char *)nmb_buffer_contents(dst);
 for(size_t i=0;i<16384;++i)assert(p[i]==(i>=8 && i<16376?0xa6:0x73));
 // A refused encoder operation returns an error; a null handle is never used.
 assert(!nmb_buffer_new(device,0,0,nullptr) && strlen(nmb_last_error()));
 assert(nmb_command_buffer_wait(nullptr,1)==-1 && strlen(nmb_last_error()));
 nmb_command_buffer_release(command);nmb_buffer_release(src);nmb_buffer_release(dst);nmb_queue_release(queue);nmb_device_release(device);
 puts("PASS: NativeMetal Rust cdylib, empty production fixtures, real fill/copy pixels/sentinels, scheduling/completion and refusal status");
}
