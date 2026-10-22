/* Isolated native offscreen CA qualification. Ignores daemon argv; no display. */
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "fuxi-metal/CACompare.h"
#ifndef FUXI_CA_CONF
#define FUXI_CA_CONF "/usr/local/share/fuxi/ca-smoke.conf"
#endif
// CARenderer is present at runtime on iOS 16 but absent from some iPhone SDK
// headers. Describe only the selectors used, and check them before dispatch.
@protocol FuxiOffscreenRenderer <NSObject>
- (void)setLayer:(CALayer *)layer;
- (void)setBounds:(CGRect)bounds;
- (void)beginFrameAtTime:(CFTimeInterval)time timeStamp:(const void *)stamp;
- (void)addUpdateRect:(CGRect)rect;
- (void)render;
- (void)endFrame;
@optional
- (CGRect)updateBounds;
@end
extern int memorystatus_control(uint32_t,int32_t,uint32_t,void *,size_t);
struct FuxiMemlimit {int32_t active;uint32_t active_attr;int32_t inactive;uint32_t inactive_attr;};
static unsigned stage;
static const char *names[]={"solid","opacity","rounded-clip","image","transform","display"};
static char timeoutLine[192],abortLine[192];static size_t timeoutLength,abortLength;
static void expired(int sig) {
 const char *line=sig==SIGALRM?timeoutLine:abortLine;size_t n=sig==SIGALRM?timeoutLength:abortLength;
 (void)write(STDOUT_FILENO,line,n);_exit(sig==SIGALRM?124:134);
}
static void arm(unsigned seconds) {
 alarm(0);
 timeoutLength=(size_t)snprintf(timeoutLine,sizeof(timeoutLine),"FUXI CA: stage %u %s FAIL watchdog=%us\n",stage,names[stage],seconds);
 abortLength=(size_t)snprintf(abortLine,sizeof(abortLine),"FUXI CA: stage %u %s FAIL signal=SIGABRT\n",stage,names[stage]);
 alarm(seconds);
}
__attribute__((noreturn)) static void fail(const char *format,...) {
 printf("FUXI CA: stage %u %s FAIL ",stage,names[stage]);va_list args;va_start(args,format);vprintf(format,args);va_end(args);putchar('\n');_exit(1);
}
static unsigned configuration(void) {
 FILE *f=fopen(FUXI_CA_CONF,"rb");if(!f)fail("config missing/unreadable");
 char raw[32];size_t n=fread(raw,1,sizeof(raw),f);int bad=ferror(f);fclose(f);
 if(bad || n==sizeof(raw))fail("config length/read");
 size_t i=0;while(i<n && raw[i] && strchr(" \t\r\n",raw[i]))++i;
 if(i==n || raw[i]<'0' || raw[i]>'5')fail("config expected max stage 0..5");
 unsigned result=(unsigned)(raw[i++]-'0');while(i<n && raw[i] && strchr(" \t\r\n",raw[i]))++i;
 if(i!=n)fail("config trailing data");return result;
}
static CGColorRef color(CGColorSpaceRef space,CGFloat r,CGFloat g,CGFloat b) {
 CGFloat v[]={r,g,b,1};CGColorRef c=CGColorCreate(space,v);if(!c)fail("color allocation");return c;
}
// mirror: the CPU reference's image rows bottom-up. CARenderer samples a layer's image
// contents with v=1 at the layer's top on an unflipped tree (observed on device: the
// image quad's top vertices carry v=1 over a linear view of the top-down CGImage
// bytes), which Metal resolves to the last row; renderInContext draws them top-down.
static CALayer *scene(unsigned n,CGColorSpaceRef space,BOOL mirror) {
 CALayer *root=[CALayer layer];root.frame=CGRectMake(0,0,256,256);root.contentsScale=1;
 CGColorRef bg=color(space,0.125,0.25,0.5);root.backgroundColor=bg;CGColorRelease(bg);
 if(n>=1) {
  CALayer *child=[CALayer layer];child.frame=CGRectMake(24,40,144,96);child.opacity=0.5;
  CGColorRef fg=color(space,0.75,0.5,0.125);child.backgroundColor=fg;CGColorRelease(fg);[root addSublayer:child];
  if(n>=2) {
   child.cornerRadius=20;child.masksToBounds=YES;
   CALayer *overflow=[CALayer layer];overflow.frame=CGRectMake(-16,48,184,64);
   CGColorRef c=color(space,0.25,0.875,0.5);overflow.backgroundColor=c;CGColorRelease(c);[child addSublayer:overflow];
  }
 }
 if(n>=3) {
  uint8_t *bytes=malloc(256*256*4);if(!bytes)fail("image allocation");
  for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x) {
   uint8_t *p=bytes+((mirror?255-y:y)*256+x)*4;p[0]=x<96?32:224;p[1]=y<160?192:48;p[2]=x<y?208:64;p[3]=255;
  }
  CGContextRef ctx=CGBitmapContextCreate(bytes,256,256,8,1024,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
  if(!ctx)fail("image bitmap context");CGImageRef image=CGBitmapContextCreateImage(ctx);if(!image)fail("image snapshot");
  CALayer *picture=[CALayer layer];picture.frame=CGRectMake(176,152,64,80);picture.contents=(id)image;
  picture.contentsGravity=kCAGravityResize;picture.minificationFilter=kCAFilterNearest;picture.magnificationFilter=kCAFilterNearest;
  [root addSublayer:picture];CGImageRelease(image);CGContextRelease(ctx);free(bytes);
  if(n>=4)picture.transform=CATransform3DMakeAffineTransform(CGAffineTransformMake(0,1,-1,0,-16,-8));
 }
 return root;
}
static void finish(id<MTLCommandBuffer>command) {
 if(!command)fail("nil completion command");[command commit];[command waitUntilCompleted];
 if(command.status!=MTLCommandBufferStatusCompleted || command.error)fail("completion status=%lu error=%ld",(unsigned long)command.status,(long)command.error.code);
}
// Display scene: root + 24 panels + 3 clipped children + 2 pictures = 30 layers.
// Geometry stays in target pixels; contentsScale=3 also exercises CA's backing
// allocations. CPU pictures use the same documented bottom-up contents oracle.
static void displayPicture(CALayer *root,CGColorSpaceRef space,BOOL mirror,unsigned width,unsigned height,CGRect frame,BOOL transform) {
 size_t row=(size_t)width*4;uint8_t *bytes=malloc(row*height);if(!bytes)fail("display image allocation");
 for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){uint8_t *p=bytes+(mirror?height-1-y:y)*row+x*4;
  p[0]=x<width/2?32:224;p[1]=y<height/2?192:48;p[2]=x<width/4?208:64;p[3]=255;}
 CGContextRef ctx=CGBitmapContextCreate(bytes,width,height,8,row,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
 if(!ctx)fail("display image context");CGImageRef image=CGBitmapContextCreateImage(ctx);if(!image)fail("display image snapshot");
 CALayer *picture=[CALayer layer];picture.frame=frame;picture.contents=(id)image;picture.contentsScale=3;
 picture.contentsGravity=kCAGravityResize;picture.minificationFilter=kCAFilterNearest;picture.magnificationFilter=kCAFilterNearest;
 if(transform)picture.transform=CATransform3DMakeAffineTransform(CGAffineTransformMake(0,1,-1,0,0,0));
 [root addSublayer:picture];CGImageRelease(image);CGContextRelease(ctx);free(bytes);
}
static CALayer *displayScene(CGColorSpaceRef space,BOOL mirror) {
 CALayer *root=[CALayer layer];root.frame=CGRectMake(0,0,1080,2400);root.contentsScale=3;
 CGColorRef bg=color(space,0.125,0.25,0.5);root.backgroundColor=bg;CGColorRelease(bg);
 for(unsigned i=0;i<24;++i){CALayer *panel=[CALayer layer];panel.contentsScale=3;
  panel.frame=CGRectMake(36+(i%4)*252,48+(i/4)*348,216,288);
  CGColorRef c=color(space,(i%3+1)*0.25,0.5,(i%2+1)*0.25);panel.backgroundColor=c;CGColorRelease(c);
  if(i%4==1)panel.opacity=0.5;
  if(i<3){panel.cornerRadius=24;panel.masksToBounds=YES;CALayer *child=[CALayer layer];child.contentsScale=3;
   child.frame=CGRectMake(-12,144,240,180);CGColorRef c2=color(space,0.25,0.875,0.5);child.backgroundColor=c2;CGColorRelease(c2);[panel addSublayer:child];}
  [root addSublayer:panel];
 }
 displayPicture(root,space,mirror,512,320,CGRectMake(60,2160,384,192),NO);
 displayPicture(root,space,mirror,128,128,CGRectMake(600,2112,320,192),YES);return root;
}
static void displayPosition(CALayer *root,unsigned frame) {
 CALayer *moving=[root.sublayers objectAtIndex:7];moving.position=CGPointMake(36+3*252+108+(frame%16),48+348+144);
}
static int compareDouble(const void *a,const void *b){double x=*(const double *)a,y=*(const double *)b;return (x>y)-(x<y);}
static void displayStage(id<MTLDevice> device,Class rendererClass,SEL factory,NSString *queueKey,NSString *spaceKey,CGColorSpaceRef space,
                         uint64_t (*failures)(void),uint64_t (*draws)(void),void *plugin) {
 uint64_t (*gpu)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_gpu_nanoseconds");
 uint64_t (*samples)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_gpu_samples");
 uint64_t (*hits)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_pipeline_cache_hits");
 uint64_t (*misses)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_pipeline_cache_misses");
 if(!gpu || !samples || !hits || !misses)fail("display diagnostic exports");
 // Optional CPU breakdown of the backend submit; absent in older plugins.
 uint64_t (*submits)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_submits");
 uint64_t (*prepare)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_submit_prepare_nanoseconds");
 uint64_t (*waited)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_submit_wait_nanoseconds");
 uint64_t (*retired)(void)=(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_submit_retire_nanoseconds");
 int breakdown=submits && prepare && waited && retired;uint64_t warmStart[4]={0},warmEnd[4]={0};
 const char *detailNames[]={"objects_nanoseconds","descriptors_nanoseconds","record_nanoseconds","shadow_nanoseconds","flush_nanoseconds","invalidate_nanoseconds","flush_calls","invalidate_calls","empty","object_creates","pool_creates","frame_creates","view_creates"};
 uint64_t (*detail[13])(void)={0};uint64_t detailStart[13]={0},detailEnd[13]={0};int detailed=1;
 for(unsigned i=0;i<13;++i){char symbol[96];snprintf(symbol,sizeof(symbol),"fuxi_metal_submit_%s",detailNames[i]);detail[i]=(uint64_t(*)(void))dlsym(plugin,symbol);if(!detail[i])detailed=0;}

 arm(60);uint64_t startDraws=draws(),startHits=hits(),startMisses=misses();double times[60],wall[60];unsigned late=0;
 // Let IOSurface choose the padded row (1080*4 need not be a Vulkan row pitch).
 IOSurfaceRef surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@1080,(id)kIOSurfaceHeight:@2400,
  (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfacePixelFormat:@0x42475241});
 if(!surface || IOSurfaceLock(surface,0,NULL))fail("display IOSurface allocation/lock");
 size_t row=IOSurfaceGetBytesPerRow(surface),length=IOSurfaceGetAllocSize(surface);void *base=IOSurfaceGetBaseAddress(surface);
 if(!base || row<4320 || row>SIZE_MAX/2400 || row*2400>length)fail("display IOSurface mapping/stride");
 memset(base,0x39,row*2400);if(IOSurfaceUnlock(surface,0,NULL))fail("display producer unlock");
 uint8_t *reference=calloc(1080*2400,4);if(!reference)fail("display reference allocation");
 @autoreleasepool {
  MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1080 height:2400 mipmapped:NO];
  td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
  id<MTLTexture> texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];id<MTLCommandQueue> queue=[device newCommandQueue];
  if(!texture || !queue)fail("display IOSurface texture/queue");
  id<FuxiOffscreenRenderer> renderer=((id(*)(id,SEL,id,id))objc_msgSend)(rendererClass,factory,texture,@{queueKey:queue,spaceKey:(id)space});
  if(!renderer)fail("display renderer");
  [CATransaction begin];[CATransaction setDisableActions:YES];CALayer *root=displayScene(space,NO);
  [renderer setLayer:root];[renderer setBounds:CGRectMake(0,0,1080,2400)];[CATransaction commit];[CATransaction flush];
  CFTimeInterval epoch=0;
  for(unsigned frame=0;frame<60;++frame){
   arm(60); // Keep each cold frame bounded; never increase the existing jetsam limit.
   CFTimeInterval begin=CACurrentMediaTime();uint64_t beforeGPU=gpu(),beforeSamples=samples(),beforeDraws=draws();
   @autoreleasepool {
    [CATransaction begin];[CATransaction setDisableActions:YES];displayPosition(root,frame);[CATransaction commit];[CATransaction flush];
    [renderer beginFrameAtTime:begin timeStamp:NULL];[renderer addUpdateRect:CGRectMake(0,0,1080,2400)];
    [renderer render];[renderer endFrame];finish(queue.commandBuffer);
   } // Include per-frame resource/autorelease retirement in wall time.
   CFTimeInterval end=CACurrentMediaTime();
   if(failures() || draws()==beforeDraws || samples()==beforeSamples)fail("display frame=%u errors/draws/timestamps",frame);
   times[frame]=(double)(gpu()-beforeGPU)/1e6;wall[frame]=(end-begin)*1000;
   if(!(times[frame]>0))fail("display frame=%u nonpositive GPU time",frame);
   // Cold frame 0 is measured separately; the following 59 frames target 60 Hz.
   if(!frame)epoch=end;
   if(breakdown && (frame==0 || frame==59)){uint64_t *v=frame?warmEnd:warmStart;v[0]=submits();v[1]=prepare();v[2]=waited();v[3]=retired();}
   if(detailed && (frame==0 || frame==59))for(unsigned i=0;i<13;++i)(frame?detailEnd:detailStart)[i]=detail[i]();
   if(frame && (wall[frame]>1000.0/60.0 || end>epoch+(frame+1)/60.0))++late;
   if(frame<59){CFTimeInterval deadline=epoch+(frame+1)/60.0,now=CACurrentMediaTime();
    if(deadline>now)usleep((useconds_t)((deadline-now)*1e6));}
  }
  [CATransaction begin];[CATransaction setDisableActions:YES];CALayer *expected=displayScene(space,YES);displayPosition(expected,59);[CATransaction commit];
  CGContextRef cpu=CGBitmapContextCreate(reference,1080,2400,8,4320,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
  if(!cpu)fail("display CPU context");CGContextTranslateCTM(cpu,0,2400);CGContextScaleCTM(cpu,1,-1);[expected renderInContext:cpu];CGContextRelease(cpu);
  [renderer setLayer:nil];renderer=nil;[texture release];[queue release];
 }
 if(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL))fail("display consumer lock");
 base=IOSurfaceGetBaseAddress(surface);struct FuxiCAComparison m=fuxiCACompareSize(base,row,reference,1080,2400);
 if(IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL))fail("display consumer unlock");
 for(unsigned i=0;i<60;++i)printf("FUXI CA: display frame=%u gpu_ms=%.3f wall_ms=%.3f\n",i,times[i],wall[i]);
 double cold=times[0];qsort(times,60,sizeof(double),compareDouble);
 double warm[59];for(unsigned i=0;i<59;++i)warm[i]=wall[i+1];qsort(warm,59,sizeof(double),compareDouble);
 printf("FUXI CA: display GPU min/median/max=%.3f/%.3f/%.3f ms cold=%.3f ms frames=60 cache_hits=%llu cache_misses=%llu\n",
  times[0],(times[29]+times[30])/2,times[59],cold,(unsigned long long)(hits()-startHits),(unsigned long long)(misses()-startMisses));
 printf("FUXI CA: display warm wall min/median/max=%.3f/%.3f/%.3f ms late=%u/59 target_hz=60\n",warm[0],warm[29],warm[58],late);
 if(breakdown){double sum=0;for(unsigned i=1;i<60;++i)sum+=wall[i];
  double p=(warmEnd[1]-warmStart[1])/1e6/59,w=(warmEnd[2]-warmStart[2])/1e6/59,r=(warmEnd[3]-warmStart[3])/1e6/59;
  printf("FUXI CA: display warm per-frame submits=%.2f prepare=%.3f wait=%.3f retire=%.3f other=%.3f wall=%.3f ms\n",
   (double)(warmEnd[0]-warmStart[0])/59,p,w,r,sum/59-p-w-r,sum/59);}
 if(detailed){double d[13];for(unsigned i=0;i<13;++i)d[i]=(double)(detailEnd[i]-detailStart[i])/59;
  printf("FUXI CA: display warm detail objects=%.3f descriptors=%.3f record=%.3f shadow=%.3f flush=%.3f invalidate=%.3f ms flush_calls=%.2f invalidate_calls=%.2f empty=%.2f creates=%.2f pool_creates=%.2f frame_creates=%.2f view_creates=%.2f\n",
   d[0]/1e6,d[1]/1e6,d[2]/1e6,d[3]/1e6,d[4]/1e6,d[5]/1e6,d[6],d[7],d[8],d[9],d[10],d[11],d[12]);}
 if(!fuxiCAAcceptSize(m,1080,2400))fail("pixels=2592000 max=%u mean=%.4f bad=%u",m.max_error,m.mean_error,m.bad_pixels);
 if(failures() || hits()==startHits)fail("display frontend errors/cache reuse missing");
 // Pixel PASS alone cannot prove display rate. Require every warm frame's render
 // and synchronous completion to fit the actual 60 Hz budget.
 if(late)fail("display 60Hz deadline missed frames=%u/59",late);
 printf("FUXI CA: stage 5 display PASS pixels=2592000 frames=60 max=%u mean=%.4f bad=%u gpu_draws=%llu\n",
  m.max_error,m.mean_error,m.bad_pixels,(unsigned long long)(draws()-startDraws));
 free(reference);CFRelease(surface);
}
int main(int argc,char **argv) {
 (void)argc;(void)argv;setvbuf(stdout,NULL,_IONBF,0);setvbuf(stderr,NULL,_IONBF,0);
 signal(SIGALRM,expired);signal(SIGABRT,expired);arm(30);
 @autoreleasepool {@try {
  if(geteuid()!=0)fail("requires unsandboxed root");unsigned maximum=configuration();
  // Exec-swapped daemons inherit a 6 MB limit. SET_MEMLIMIT_PROPERTIES is 7,
  // not CONFIG_JETSAM-only command 6. Refuse to compile if raising it fails.
  struct FuxiMemlimit before={0},limit={1024,0,1024,0};
  int get=memorystatus_control(8,getpid(),0,&before,sizeof(before));
  int set=memorystatus_control(7,getpid(),0,&limit,sizeof(limit));int saved=set?errno:0;
  printf("FUXI CA: jetsam before=%d/%dMB get=%d set=1024MB rc=%d errno=%d\n",before.active,before.inactive,get,set,saved);
  if(set)fail("jetsam limit raise failed");
  id<MTLDevice>device=MTLCreateSystemDefaultDevice();
  if(!device || strcmp(object_getClassName(device),"FuxiMTLDevice"))fail("Fuxi Metal device required");
  void *plugin=dlopen("/System/Library/Extensions/FuxiMetal.bundle/FuxiMetal",RTLD_NOW|RTLD_NOLOAD);
  uint64_t (*failures)(void)=plugin?(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_failure_count"):NULL;
  uint64_t (*draws)(void)=plugin?(uint64_t(*)(void))dlsym(plugin,"fuxi_metal_completed_draws"):NULL;
  if(!failures || !draws || failures())fail("diagnostic exports/init errors");
  Class rendererClass=NSClassFromString(@"CARenderer");SEL factory=NSSelectorFromString(@"rendererWithMTLTexture:options:");
  if(!rendererClass || ![rendererClass respondsToSelector:factory])fail("unsupported selector CARenderer rendererWithMTLTexture:options:");
  // Resolve the actual exported keys; do not guess their NSString values.
  NSString **queueKey=(NSString **)dlsym(RTLD_DEFAULT,"kCARendererMetalCommandQueue");
  NSString **spaceKey=(NSString **)dlsym(RTLD_DEFAULT,"kCARendererColorSpace");
  if(!queueKey || !*queueKey || !spaceKey || !*spaceKey)fail("CARenderer option keys unavailable");
  CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);if(!space)fail("sRGB CPU color space");
  for(stage=0;stage<=maximum && stage<5;++stage) {@autoreleasepool {
   arm(60);uint64_t startDraws=draws();
   IOSurfaceRef surface=IOSurfaceCreate((CFDictionaryRef)@{(id)kIOSurfaceWidth:@256,(id)kIOSurfaceHeight:@256,
    (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@1024,(id)kIOSurfacePixelFormat:@0x42475241});
   if(!surface || IOSurfaceLock(surface,0,NULL))fail("IOSurface allocation/lock");
   size_t row=IOSurfaceGetBytesPerRow(surface);uint8_t *base=IOSurfaceGetBaseAddress(surface);
   if(!base || row<1024)fail("IOSurface mapping/stride");memset(base,0x39,row*256);
   if(IOSurfaceUnlock(surface,0,NULL))fail("IOSurface unlock");
   uint8_t *reference=NULL,*mirrored=NULL;uint64_t rendered=0;
   @autoreleasepool {
   MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
   td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead|MTLTextureUsageRenderTarget;
   id<MTLTexture>texture=[device newTextureWithDescriptor:td iosurface:surface plane:0];
   id<MTLCommandQueue>queue=[device newCommandQueue];if(!texture || !queue)fail("IOSurface texture/queue");
   id<FuxiOffscreenRenderer>renderer=((id(*)(id,SEL,id,id))objc_msgSend)(rendererClass,factory,texture,@{*queueKey:queue,*spaceKey:(id)space});
   if(!renderer)fail("nil CARenderer");
   for(NSString *selector in @[@"setLayer:",@"setBounds:",@"beginFrameAtTime:timeStamp:",@"addUpdateRect:",@"render",@"endFrame"])
    if(![renderer respondsToSelector:NSSelectorFromString(selector)])fail("unsupported selector CARenderer %.96s",selector.UTF8String);
   [CATransaction begin];[CATransaction setDisableActions:YES];CALayer *root=scene(stage,space,NO);
   [renderer setLayer:root];[renderer setBounds:CGRectMake(0,0,256,256)];[CATransaction commit];
   // Stage 1 on device drew nothing until the transaction was flushed to the renderer.
   [CATransaction flush];
   reference=calloc(256*256,4);if(!reference)fail("reference allocation");
   CGContextRef cpu=CGBitmapContextCreate(reference,256,256,8,1024,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
   if(!cpu)fail("reference context");
   // iOS layer geometry has a top-left origin, as UIKit's flipped bitmap contexts do;
   // a raw CGBitmapContext is bottom-left. Keep the unflipped render for diagnostics.
   CALayer *expected=scene(stage,space,YES);
   CGContextTranslateCTM(cpu,0,256);CGContextScaleCTM(cpu,1,-1);[expected renderInContext:cpu];CGContextRelease(cpu);
   mirrored=calloc(256*256,4);if(!mirrored)fail("reference allocation");
   cpu=CGBitmapContextCreate(mirrored,256,256,8,1024,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
   if(!cpu)fail("reference context");[expected renderInContext:cpu];CGContextRelease(cpu);
   uint64_t beforeRender=draws();
   [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];[renderer addUpdateRect:CGRectMake(0,0,256,256)];
   if([renderer respondsToSelector:@selector(updateBounds)]) {CGRect u=[renderer updateBounds];
    printf("FUXI CA: stage %u update=%.0f,%.0f %.0fx%.0f\n",stage,u.origin.x,u.origin.y,u.size.width,u.size.height);}
   [renderer render];[renderer endFrame];finish(queue.commandBuffer);
   rendered=draws()-beforeRender;
   // Diagnose a frame CA declined to draw: retry after a flush, then with a fresh renderer.
   for(unsigned retry=1;!rendered && retry<=2;++retry) {
    if(retry==2) {[renderer setLayer:nil];
     renderer=((id(*)(id,SEL,id,id))objc_msgSend)(rendererClass,factory,texture,@{*queueKey:queue,*spaceKey:(id)space});
     if(!renderer)fail("nil retry CARenderer");
     [CATransaction begin];[CATransaction setDisableActions:YES];[renderer setLayer:root];[renderer setBounds:CGRectMake(0,0,256,256)];[CATransaction commit];}
    [CATransaction flush];uint64_t again=draws();
    [renderer beginFrameAtTime:CACurrentMediaTime() timeStamp:NULL];[renderer addUpdateRect:CGRectMake(0,0,256,256)];
    [renderer render];[renderer endFrame];finish(queue.commandBuffer);
    rendered=draws()-again;
    printf("FUXI CA: stage %u retry %u (%s) gpu_draws=%llu\n",stage,retry,retry==1?"flush":"new renderer",(unsigned long long)rendered);
   }
   if(failures() || !rendered)fail("frontend_errors=%llu gpu_draws=%llu",(unsigned long long)failures(),(unsigned long long)rendered);
   // Release CA's imported target before taking CPU ownership. The frontend's
   // current submit is synchronous; the supplied queue also has a completion fence.
   [renderer setLayer:nil];renderer=nil;[texture release];[queue release];
   } // Drain CA and all imported texture retains before the CPU consumer lock.
   if(IOSurfaceLock(surface,kIOSurfaceLockReadOnly,NULL))fail("consumer lock");
   base=IOSurfaceGetBaseAddress(surface);if(!base)fail("consumer mapping");
   struct FuxiCAComparison m=fuxiCACompare(base,row,reference),u=fuxiCACompare(base,row,mirrored);
   const uint8_t *top=base+60*row+96*4,*bottom=base+196*row+96*4;
   printf("FUXI CA: stage %u orientation top-left bad=%u unflipped bad=%u probe(96,60)=%02x%02x%02x%02x probe(96,196)=%02x%02x%02x%02x\n",
    stage,m.bad_pixels,u.bad_pixels,top[2],top[1],top[0],top[3],bottom[2],bottom[1],bottom[0],bottom[3]);
   if(IOSurfaceUnlock(surface,kIOSurfaceLockReadOnly,NULL))fail("consumer unlock");
   if(!fuxiCAAccept(m)) {
    // Locate the mismatch: bounding box of bad pixels and a 4x4 grid of GPU/CPU samples in it (ARGB hex).
    unsigned x0=256,y0=256,x1=0,y1=0;
    for(unsigned y=0;y<256;++y)for(unsigned x=0;x<256;++x)for(unsigned c=0;c<4;++c) {
     int d=(int)base[y*row+x*4+c]-(int)reference[(y*256+x)*4+c];
     if(d>8 || d<-8){if(x<x0)x0=x;if(x>x1)x1=x;if(y<y0)y0=y;if(y>y1)y1=y;break;}
    }
    printf("FUXI CA: stage %u bad box x=%u..%u y=%u..%u\n",stage,x0,x1,y0,y1);
    for(unsigned j=0;x0<=x1 && j<4;++j) {printf("FUXI CA: stage %u row",stage);
     for(unsigned i=0;i<4;++i) {unsigned x=x0+(x1-x0)*(2*i+1)/8,y=y0+(y1-y0)*(2*j+1)/8;
      const uint8_t *g=base+y*row+x*4,*c=reference+(y*256+x)*4;
      printf(" (%u,%u) gpu=%02x%02x%02x%02x cpu=%02x%02x%02x%02x",x,y,g[3],g[2],g[1],g[0],c[3],c[2],c[1],c[0]);}
     putchar('\n');}
   }
   if(!fuxiCAAccept(m))fail("pixels=65536 max=%u mean=%.4f bad=%u tol=8/64/256/1",m.max_error,m.mean_error,m.bad_pixels);
   if(failures())fail("frontend_errors=%llu",(unsigned long long)failures());
   printf("FUXI CA: stage %u %s PASS pixels=65536 max=%u mean=%.4f bad=%u gpu_draws=%llu total_draws=%llu\n",stage,names[stage],m.max_error,m.mean_error,m.bad_pixels,(unsigned long long)rendered,(unsigned long long)(draws()-startDraws));
   free(reference);free(mirrored);CFRelease(surface);
  }}
  if(maximum==5){stage=5;displayStage(device,rendererClass,factory,*queueKey,*spaceKey,space,failures,draws,plugin);}
  stage=maximum;arm(10);CGColorSpaceRelease(space);[device release];dlclose(plugin);alarm(0);puts("FUXI CA: done PASS");
 }@catch(NSException *exception) {fail("exception %.96s",exception.name.UTF8String);}}
 return 0;
}
