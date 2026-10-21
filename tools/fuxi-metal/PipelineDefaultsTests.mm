/* Actual external library/function metadata + iOS public-default regression.
 * CPU model stops at backend compilation; no iOS/GPU success is claimed. */
#define FUXI_MTL_FRONTEND_TEST
#define FUXI_MTL_BACKEND_HEADER "TestBackend.hpp"
#include "Frontend.mm"
#include "IOS16Defaults.hpp"
#include <cassert>
@interface FuxiDefaultsTestBase : NSObject {uint8_t bytes[520];}
@end
@implementation FuxiDefaultsTestBase
@end
static void rejects(MTLRenderPipelineDescriptor *d,id device,NSString *field,void (^change)(MTLRenderPipelineDescriptor *)) {
 MTLRenderPipelineDescriptor *copy=[[d copy] autorelease];change(copy);NSString *actual=pipelineRejection(copy,device);
 assert([actual isEqualToString:field]);
}
int main(int argc,char **argv) {assert(argc==2);@autoreleasepool {
 Class cls=makeDeviceClass([FuxiDefaultsTestBase class]);assert(cls);
 FuxiOwner *state=[FuxiOwner new];state->backend=new fuxi::Backend;state->registry=123;
 id device=[cls alloc];objc_setAssociatedObject(device,&ownerKey,state,OBJC_ASSOCIATION_RETAIN_NONATOMIC);[state release];
 NSError *error=nil;NSURL *url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
 FuxiLibrary *vl=[device newLibraryWithURL:url error:&error];assert(vl && !error);
 NSData *bytes=[NSData dataWithContentsOfURL:url];
 dispatch_data_t data=dispatch_data_create(bytes.bytes,bytes.length,dispatch_get_global_queue(0,0),DISPATCH_DATA_DESTRUCTOR_DEFAULT);
 FuxiLibrary *fl=[device newLibraryWithData:data error:&error];dispatch_release(data);assert(fl && !error);
 FuxiFunction *v=[vl newFunctionWithName:@"fuxi_vertex"],*f=[fl newFunctionWithName:@"fuxi_fragment"];
 assert([v isKindOfClass:[FuxiFunction class]] && [f isKindOfClass:[FuxiFunction class]]);
 assert(v->_stage==1 && f->_stage==2 && [v device]==device && [f device]==device);
 assert([v functionType]==MTLFunctionTypeVertex && [f functionType]==MTLFunctionTypeFragment);
 auto d=ios16PipelineDefaults();d.vertexFunction=(id)v;d.fragmentFunction=(id)f;d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 assert(d.vertexDescriptor); // M5b's blanket non-nil check rejects this real default.
 assert(!pipelineRejection(d,device));
 assert(![device newRenderPipelineStateWithDescriptor:d error:&error]);
 assert([error.localizedDescription isEqualToString:@"M4 CPU model has no translation"]); // Reached backend.
 rejects(d,device,@"vertexFunction.stage",^(MTLRenderPipelineDescriptor *x){x.vertexFunction=(id)f;});
 rejects(d,device,@"fragmentFunction.stage",^(MTLRenderPipelineDescriptor *x){x.fragmentFunction=(id)v;});
 rejects(d,device,@"vertexFunction.class",^(MTLRenderPipelineDescriptor *x){x.vertexFunction=nil;});
 rejects(d,device,@"fragmentFunction.class",^(MTLRenderPipelineDescriptor *x){x.fragmentFunction=nil;});
 FuxiLibrary *foreign=[FuxiLibrary new];foreign->_device=[NSObject new];
 FuxiFunction *fv=[FuxiFunction new];fv->_library=foreign;fv->_name=[@"fuxi_vertex" copy];fv->_stage=1;
 rejects(d,device,@"vertexFunction.device",^(MTLRenderPipelineDescriptor *x){x.vertexFunction=(id)fv;});[fv release];
 rejects(d,device,@"vertexDescriptor.attributes[30].format",^(MTLRenderPipelineDescriptor *x){x.vertexDescriptor.attributes[30].format=MTLVertexFormatFloat4;});
 rejects(d,device,@"vertexDescriptor.layouts[30].stride",^(MTLRenderPipelineDescriptor *x){x.vertexDescriptor.layouts[30].stride=16;});
 rejects(d,device,@"rasterSampleCount",^(MTLRenderPipelineDescriptor *x){x.rasterSampleCount=4;});
 rejects(d,device,@"alphaToCoverageEnabled",^(MTLRenderPipelineDescriptor *x){x.alphaToCoverageEnabled=YES;});
 rejects(d,device,@"alphaToOneEnabled",^(MTLRenderPipelineDescriptor *x){x.alphaToOneEnabled=YES;});
 rejects(d,device,@"rasterizationEnabled",^(MTLRenderPipelineDescriptor *x){x.rasterizationEnabled=NO;});
 rejects(d,device,@"depthAttachmentPixelFormat",^(MTLRenderPipelineDescriptor *x){x.depthAttachmentPixelFormat=MTLPixelFormatDepth32Float;});
 rejects(d,device,@"stencilAttachmentPixelFormat",^(MTLRenderPipelineDescriptor *x){x.stencilAttachmentPixelFormat=MTLPixelFormatStencil8;});
 rejects(d,device,@"inputPrimitiveTopology",^(MTLRenderPipelineDescriptor *x){x.inputPrimitiveTopology=MTLPrimitiveTopologyClassLine;});
 rejects(d,device,@"maxVertexAmplificationCount",^(MTLRenderPipelineDescriptor *x){x.maxVertexAmplificationCount=2;});
 rejects(d,device,@"vertexLinkedFunctions.functions",^(MTLRenderPipelineDescriptor *x){x.vertexLinkedFunctions.functions=@[(id<MTLFunction>)v];});
 rejects(d,device,@"fragmentLinkedFunctions.functions",^(MTLRenderPipelineDescriptor *x){x.fragmentLinkedFunctions.functions=@[(id<MTLFunction>)f];});
 rejects(d,device,@"colorAttachments[0].pixelFormat",^(MTLRenderPipelineDescriptor *x){x.colorAttachments[0].pixelFormat=MTLPixelFormatRGBA8Unorm;});
 rejects(d,device,@"colorAttachments[0].blendingEnabled",^(MTLRenderPipelineDescriptor *x){x.colorAttachments[0].blendingEnabled=YES;});
 rejects(d,device,@"colorAttachments[0].writeMask",^(MTLRenderPipelineDescriptor *x){x.colorAttachments[0].writeMask=MTLColorWriteMaskRed;});
 rejects(d,device,@"colorAttachments[7].pixelFormat",^(MTLRenderPipelineDescriptor *x){x.colorAttachments[7].pixelFormat=MTLPixelFormatBGRA8Unorm;});
 d.vertexDescriptor=nil;d.vertexLinkedFunctions=nil;d.fragmentLinkedFunctions=nil;assert(!pipelineRejection(d,device));
 d.maxTessellationFactor=64;d.supportIndirectCommandBuffers=YES;d.inputPrimitiveTopology=MTLPrimitiveTopologyClassTriangle;
 d.colorAttachments[7].blendingEnabled=YES;d.colorAttachments[7].writeMask=MTLColorWriteMaskNone;
 assert(!pipelineRejection(d,device)); // Inert fields / compatible triangle hint.
 auto td=ios16SmokeTexture();assert(![device newTextureWithDescriptor:td]); // Reaches modeled allocation.
 td.allowGPUOptimizedContents=NO;assert(![device newTextureWithDescriptor:td]); // Hint is inert.
 auto pass=ios16PassDefaults();assert(!pass.colorAttachments[0].texture && !pass.defaultRasterSampleCount);
 [v release];[f release];[vl release];[fl release];[device release];
 }assert(fuxi::Backend::live==0);
 puts("PASS: 20H392 defaults pass pipeline gate; real FuxiLibrary URL/data functions have stages 1/2 and owning device; rendering changes reject by name (no GPU submit)");
}
