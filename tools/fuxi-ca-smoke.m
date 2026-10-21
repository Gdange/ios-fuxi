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
static const char *names[]={"solid","opacity","rounded-clip","image","transform"};
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
 if(i==n || raw[i]<'0' || raw[i]>'4')fail("config expected max stage 0..4");
 unsigned result=(unsigned)(raw[i++]-'0');while(i<n && raw[i] && strchr(" \t\r\n",raw[i]))++i;
 if(i!=n)fail("config trailing data");return result;
}
static CGColorRef color(CGColorSpaceRef space,CGFloat r,CGFloat g,CGFloat b) {
 CGFloat v[]={r,g,b,1};CGColorRef c=CGColorCreate(space,v);if(!c)fail("color allocation");return c;
}
static CALayer *scene(unsigned n,CGColorSpaceRef space) {
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
   uint8_t *p=bytes+(y*256+x)*4;p[0]=x<96?32:224;p[1]=y<160?192:48;p[2]=x<y?208:64;p[3]=255;
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
  for(stage=0;stage<=maximum;++stage) {@autoreleasepool {
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
   [CATransaction begin];[CATransaction setDisableActions:YES];CALayer *root=scene(stage,space);
   [renderer setLayer:root];[renderer setBounds:CGRectMake(0,0,256,256)];[CATransaction commit];
   // Stage 1 on device drew nothing until the transaction was flushed to the renderer.
   [CATransaction flush];
   reference=calloc(256*256,4);if(!reference)fail("reference allocation");
   CGContextRef cpu=CGBitmapContextCreate(reference,256,256,8,1024,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
   if(!cpu)fail("reference context");
   // iOS layer geometry has a top-left origin, as UIKit's flipped bitmap contexts do;
   // a raw CGBitmapContext is bottom-left. Keep the unflipped render for diagnostics.
   CGContextTranslateCTM(cpu,0,256);CGContextScaleCTM(cpu,1,-1);[root renderInContext:cpu];CGContextRelease(cpu);
   mirrored=calloc(256*256,4);if(!mirrored)fail("reference allocation");
   cpu=CGBitmapContextCreate(mirrored,256,256,8,1024,space,kCGBitmapByteOrder32Little|kCGImageAlphaPremultipliedFirst);
   if(!cpu)fail("reference context");[root renderInContext:cpu];CGContextRelease(cpu);
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
  stage=maximum;arm(10);CGColorSpaceRelease(space);[device release];dlclose(plugin);alarm(0);puts("FUXI CA: done PASS");
 }@catch(NSException *exception) {fail("exception %.96s",exception.name.UTF8String);}}
 return 0;
}
