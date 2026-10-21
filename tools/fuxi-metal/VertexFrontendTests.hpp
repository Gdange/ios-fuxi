/* Public descriptors synthesized from the static CA layout facts; no Apple
 * shader data or GPU execution. Included by the existing native test TUs. */
#pragma once
static void checkVertexDescriptors(id device,FuxiFunction *vertex,FuxiFunction *fragment) {
 auto d=ios16PipelineDefaults();d.vertexFunction=(id)vertex;d.fragmentFunction=(id)fragment;
 d.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
 for(unsigned layout=0;layout<3;++layout) {
  d.vertexDescriptor=[MTLVertexDescriptor vertexDescriptor];auto vd=d.vertexDescriptor;
  auto set=[&](unsigned index,MTLVertexFormat format,NSUInteger offset){auto a=vd.attributes[index];a.format=format;a.offset=offset;a.bufferIndex=1;};
  vd.layouts[1].stride=layout?16:48;
  set(0,layout?MTLVertexFormatFloat2:MTLVertexFormatFloat4,0);
  if(!layout){set(1,MTLVertexFormatFloat2,16);set(2,MTLVertexFormatFloat2,24);set(3,MTLVertexFormatHalf4,32);}
  else if(layout==1)set(3,MTLVertexFormatHalf4,8);
  else {set(4,MTLVertexFormatFloat,8);set(3,MTLVertexFormatUChar4Normalized,12);}
  assert(!pipelineRejection(d,device));
  NSString *summary=vertexDescriptorText(vd);
  assert([summary containsString:@"attribute[3]"] && [summary containsString:@"bufferIndex=1"]);
  assert([summary containsString:layout==2?@"format=9(UChar4Normalized)":@"format=27(Half4)"]);
  assert([summary containsString:layout?@"1:stride=16/stepFunction=1/stepRate=1":@"1:stride=48/stepFunction=1/stepRate=1"]);
  NSError *error=nil;assert(![device newRenderPipelineStateWithDescriptor:d error:&error]);
  assert([error.localizedDescription isEqualToString:@"M4 CPU model has no translation"]);
 }
 auto vd=[MTLVertexDescriptor vertexDescriptor];
 vd.attributes[7].bufferIndex=3;vd.layouts[3].stride=256;
 for(unsigned format=1;format<=54;++format) {
  if(format==43 || format==44)continue;
  vd.attributes[7].format=(MTLVertexFormat)format;
  vd.attributes[7].offset=256-fuxi::vertexFormatInfo(format).bytes;
  assert(!vertexDescriptorRejection(vd));
  ++vd.attributes[7].offset;assert([vertexDescriptorRejection(vd) containsString:@"offset + format bytes > 256"]);
 }
 vd.attributes[7].format=MTLVertexFormatHalf4;vd.attributes[7].offset=32;
 vd.layouts[3].stepFunction=MTLVertexStepFunctionPerInstance;
 NSString *why=vertexDescriptorRejection(vd);
 for(NSString *field in @[@"layout[3]",@"attribute[7]",@"format=27(Half4)",@"bufferIndex=3",@"offset=32",@"stride=256",@"stepFunction=2",@"stepRate=1",@"requires PerVertex"])assert([why containsString:field]);
 vd.layouts[3].stepFunction=MTLVertexStepFunctionPerVertex;vd.layouts[3].stepRate=2;
 assert([vertexDescriptorRejection(vd) containsString:@"stepRate requires 1"]);
 vd.layouts[3].stepRate=1;vd.layouts[3].stride=0;
 assert([vertexDescriptorRejection(vd) containsString:@"stride outside 1..256"]);
 vd.layouts[3].stride=256;vd.attributes[7].bufferIndex=30; // Metal itself asserts on 31
 assert([vertexDescriptorRejection(vd) containsString:@"bufferIndex=30"]);
 assert([vertexDescriptorRejection(vd) containsString:@"bufferIndex >= 4"]);
 vd.attributes[7].bufferIndex=3;vd.attributes[7].format=(MTLVertexFormat)55;
 assert([vertexDescriptorRejection(vd) containsString:@"FloatRGB9E5"]);
 vd.attributes[7].format=MTLVertexFormatInvalid;vd.layouts[3].stride=0;
 vd.layouts[30].stepFunction=MTLVertexStepFunctionConstant;vd.layouts[30].stepRate=0;
 assert([vertexDescriptorRejection(vd) containsString:@"layout[30]"]);
 assert([vertexDescriptorText(vd) containsString:@"30:stride=0/stepFunction=0/stepRate=0"]);
 vd.layouts[30].stepFunction=MTLVertexStepFunctionPerVertex;vd.layouts[30].stepRate=1;
 vd.layouts[0].stepFunction=MTLVertexStepFunctionConstant;vd.layouts[0].stepRate=0;
 assert([vertexDescriptorRejection(vd) containsString:@"requires PerVertex"]);
 assert(!vertexDescriptorRejection(nil));
}
