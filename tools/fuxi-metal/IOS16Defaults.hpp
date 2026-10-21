/* Public-value fixtures derived from 20H392 init/reset/getter analysis.
 * No private object layout or Apple implementation is used. See docs-lanes/metal.md.
 * Explicit assignments keep a newer host framework from supplying its defaults. */
#pragma once
[[maybe_unused]] static MTLRenderPipelineDescriptor *ios16PipelineDefaults() {
 auto d=[[MTLRenderPipelineDescriptor new] autorelease];
 d.vertexFunction=nil;d.fragmentFunction=nil;
 // iOS lazily creates this on first read. Model its public empty shape.
 auto vd=[MTLVertexDescriptor vertexDescriptor];
 for(unsigned i=0;i<31;++i) {
  vd.attributes[i].format=MTLVertexFormatInvalid;vd.attributes[i].offset=0;vd.attributes[i].bufferIndex=0;
  vd.layouts[i].stride=0;vd.layouts[i].stepFunction=MTLVertexStepFunctionPerVertex;vd.layouts[i].stepRate=1;
 }
 d.vertexDescriptor=vd;
 d.rasterSampleCount=1;d.alphaToCoverageEnabled=NO;d.alphaToOneEnabled=NO;d.rasterizationEnabled=YES;
 d.depthAttachmentPixelFormat=MTLPixelFormatInvalid;d.stencilAttachmentPixelFormat=MTLPixelFormatInvalid;
 d.inputPrimitiveTopology=MTLPrimitiveTopologyClassUnspecified;d.maxVertexAmplificationCount=1;
 d.tessellationFactorFormat=MTLTessellationFactorFormatHalf;d.maxTessellationFactor=16;d.supportIndirectCommandBuffers=NO;
 // These getters also lazily allocate empty objects on iOS. Their collections
 // start nil; nil objects and empty collections are equally inert here.
 auto linked=[[MTLLinkedFunctions new] autorelease];
 linked.functions=nil;linked.binaryFunctions=nil;linked.privateFunctions=nil;linked.groups=nil;
 d.vertexLinkedFunctions=linked;d.fragmentLinkedFunctions=linked;
 for(unsigned i=0;i<8;++i) {
  auto a=d.colorAttachments[i];a.pixelFormat=MTLPixelFormatInvalid;a.blendingEnabled=NO;a.writeMask=MTLColorWriteMaskAll;
 }
 return d;
}
[[maybe_unused]] static MTLRenderPassDescriptor *ios16PassDefaults() {
 auto d=[MTLRenderPassDescriptor renderPassDescriptor];
 d.visibilityResultBuffer=nil;d.renderTargetArrayLength=0;d.imageblockSampleLength=0;d.tileWidth=0;d.tileHeight=0;
 d.threadgroupMemoryLength=0;d.defaultRasterSampleCount=0;d.rasterizationRateMap=nil;d.renderTargetWidth=0;d.renderTargetHeight=0;
 [d setSamplePositions:nullptr count:0];
 d.depthAttachment.texture=nil;d.stencilAttachment.texture=nil;d.depthAttachment.resolveTexture=nil;d.stencilAttachment.resolveTexture=nil;
 for(unsigned i=0;i<8;++i) {
  auto a=d.colorAttachments[i];a.texture=nil;a.level=0;a.slice=0;a.depthPlane=0;a.resolveTexture=nil;
  a.loadAction=MTLLoadActionDontCare;a.storeAction=MTLStoreActionStore;
  a.clearColor=MTLClearColorMake(0,0,0,1);
 }
 return d;
}
[[maybe_unused]] static MTLTextureDescriptor *ios16SmokeTexture() {
 auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
 d.textureType=MTLTextureType2D;d.depth=1;d.mipmapLevelCount=1;d.arrayLength=1;d.sampleCount=1;
 d.cpuCacheMode=MTLCPUCacheModeDefaultCache;d.hazardTrackingMode=MTLHazardTrackingModeDefault;d.allowGPUOptimizedContents=YES;
 // Explicit changes in the existing smoke, after texture2DDescriptor/init.
 d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;
 return d;
}
