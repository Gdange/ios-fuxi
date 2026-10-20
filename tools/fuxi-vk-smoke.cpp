/* Unsandboxed root, direct ICD smoke. Host argv is deliberately ignored. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include "vk-smoke-spv.h"
#ifndef FUXI_SMOKE_ICD
#define FUXI_SMOKE_ICD "/usr/local/lib/libvulkan_freedreno.dylib"
#endif
#ifndef FUXI_SMOKE_CONF
#define FUXI_SMOKE_CONF "/usr/local/share/fuxi/vk-smoke.conf"
#endif
static unsigned stage;
static const char *name="setup";
static char alarm_line[192];
static size_t alarm_length;
static void expired(int) { (void)!write(STDOUT_FILENO,alarm_line,alarm_length); _exit(124); }
static void arm(unsigned seconds, const char *operation) {
 alarm(0);
 alarm_length=(size_t)snprintf(alarm_line,sizeof(alarm_line),
     "FUXI VK: stage %u %s FAIL %s exceeded %u s wall bound; exiting\n",stage,name,operation,seconds);
 alarm(seconds);
}
static void report(const char *status, const char *fmt, ...) {
 printf("FUXI VK: stage %u %s %s ",stage,name,status);
 va_list args;va_start(args,fmt);vprintf(fmt,args);va_end(args);putchar('\n');
}
[[noreturn]] static void fail(const char *fmt, ...) {
 printf("FUXI VK: stage %u %s FAIL ",stage,name);
 va_list args;va_start(args,fmt);vprintf(fmt,args);va_end(args);putchar('\n');
 /* Avoid driver teardown waits after failure. Process death closes the client;
  * the independent kernel watchdog owns containment/quarantine. */
 _exit(1);
}
static void check(VkResult rc,const char *op) { if(rc!=VK_SUCCESS) fail("%s rc %d",op,rc); }
#define CHECK(x) check((x),#x)
static double now() { struct timespec t={};clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
#define DEVICE_FUNCTIONS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
 X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
 X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(ResetCommandPool) X(BeginCommandBuffer) X(EndCommandBuffer) \
 X(CmdFillBuffer) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences) X(DeviceWaitIdle) \
 X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
 X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CreateComputePipelines) X(DestroyPipeline) \
 X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdDispatch) X(CreateImage) X(DestroyImage) \
 X(GetImageMemoryRequirements) X(BindImageMemory) X(CreateImageView) X(DestroyImageView) \
 X(CreateRenderPass) X(DestroyRenderPass) X(CreateFramebuffer) X(DestroyFramebuffer) \
 X(CreateGraphicsPipelines) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdDraw) X(CmdCopyImageToBuffer)
struct Context {
 VkInstance instance=VK_NULL_HANDLE; VkPhysicalDevice physical=VK_NULL_HANDLE;
 VkDevice device=VK_NULL_HANDLE; VkQueue queue=VK_NULL_HANDLE;
 VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer cmd=VK_NULL_HANDLE;
 VkPhysicalDeviceMemoryProperties memory={};
 PFN_vkDestroyInstance DestroyInstance=nullptr;
#define FIELD(n) PFN_vk##n n=nullptr;
 DEVICE_FUNCTIONS(FIELD)
#undef FIELD
 void open(PFN_vkGetInstanceProcAddr gipa) {
#define INSTANCE(n) auto n=reinterpret_cast<PFN_vk##n>(gipa(instance,"vk" #n)); if(!n) fail("missing vk%s",#n)
  INSTANCE(CreateInstance);
  VkApplicationInfo app={};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName="fuxi-vk-smoke";app.apiVersion=VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci={};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pApplicationInfo=&app;
  CHECK(CreateInstance(&ci,nullptr,&instance));
  DestroyInstance=reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance,"vkDestroyInstance"));
  if(!DestroyInstance)fail("missing vkDestroyInstance");
  INSTANCE(EnumeratePhysicalDevices);INSTANCE(GetPhysicalDeviceMemoryProperties);
  INSTANCE(GetPhysicalDeviceQueueFamilyProperties);INSTANCE(CreateDevice);INSTANCE(GetDeviceProcAddr);
  uint32_t n=0;CHECK(EnumeratePhysicalDevices(instance,&n,nullptr));
  if(!n || n>16)fail("physical-device-count %u",n);
  std::vector<VkPhysicalDevice> devices(n);CHECK(EnumeratePhysicalDevices(instance,&n,devices.data()));physical=devices[0];
  GetPhysicalDeviceMemoryProperties(physical,&memory);
  n=0;GetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);if(!n || n>64)fail("queue-family-count %u",n);
  std::vector<VkQueueFamilyProperties> families(n);GetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
  uint32_t family=0;
  const VkQueueFlags required=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;
  while(family<n && (!families[family].queueCount || (families[family].queueFlags&required)!=required))++family;
  if(family==n)fail("no graphics+compute queue");
  float priority=1;
  VkDeviceQueueCreateInfo qi={};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
  VkDeviceCreateInfo di={};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
  CHECK(CreateDevice(physical,&di,nullptr,&device));
#define LOAD(n) n=reinterpret_cast<PFN_vk##n>(GetDeviceProcAddr(device,"vk" #n));if(!n)fail("missing vk%s",#n);
  DEVICE_FUNCTIONS(LOAD)
#undef LOAD
#undef INSTANCE
  GetDeviceQueue(device,family,0,&queue);if(!queue)fail("null queue");
  VkCommandPoolCreateInfo pi={};pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.queueFamilyIndex=family;
  CHECK(CreateCommandPool(device,&pi,nullptr,&pool));
  VkCommandBufferAllocateInfo ai={};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
  CHECK(AllocateCommandBuffers(device,&ai,&cmd));
 }
 uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
  for(uint32_t i=0;i<memory.memoryTypeCount;++i)
   if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags)return i;
  fail("no memory type bits 0x%x flags 0x%x",bits,flags);
 }
 VkDeviceMemory allocate(VkMemoryRequirements req,VkMemoryPropertyFlags flags) {
  VkMemoryAllocateInfo ai={};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize=req.size;ai.memoryTypeIndex=memory_type(req.memoryTypeBits,flags);
  VkDeviceMemory out;CHECK(AllocateMemory(device,&ai,nullptr,&out));return out;
 }
 void begin() {
  CHECK(ResetCommandPool(device,pool,0));
  VkCommandBufferBeginInfo bi={};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  CHECK(BeginCommandBuffer(cmd,&bi));
 }
 void host_barrier(VkAccessFlags access,VkPipelineStageFlags source) {
  VkMemoryBarrier b={};b.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;b.srcAccessMask=access;b.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
  CmdPipelineBarrier(cmd,source,VK_PIPELINE_STAGE_HOST_BIT,0,1,&b,0,nullptr,0,nullptr);
 }
 void submit(const char *label) {
  CHECK(EndCommandBuffer(cmd));
  VkFenceCreateInfo fi={};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;CHECK(CreateFence(device,&fi,nullptr,&fence));
  VkSubmitInfo si={};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&cmd;
  double start=now();printf("FUXI VK: submit %s BEGIN\n",label);
  CHECK(QueueSubmit(queue,1,&si,fence));
  VkResult rc=WaitForFences(device,1,&fence,VK_TRUE,2000000000ULL);
  printf("FUXI VK: fence %s rc %d elapsed-ms %.3f\n",label,rc,(now()-start)*1000);
  if(rc==VK_TIMEOUT) {
   report("FAIL","fence timeout %s elapsed-ms %.3f; bounded idle next",label,(now()-start)*1000);
   /* Vulkan has no timed DeviceWaitIdle. SIGALRM terminates this one attempt
    * after 1 s, with no destructor/join that could hang on the driver. */
   arm(1,"vkDeviceWaitIdle after fence timeout");
   rc=DeviceWaitIdle(device);
   printf("FUXI VK: bounded-idle rc %d elapsed-ms %.3f; exiting\n",rc,(now()-start)*1000);
   _exit(1);
  }
  check(rc,"vkWaitForFences");DestroyFence(device,fence,nullptr);
 }
 template<size_t N> VkShaderModule shader(const uint32_t (&words)[N]) {
  VkShaderModuleCreateInfo ci={};ci.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;ci.codeSize=sizeof(words);ci.pCode=words;
  VkShaderModule module;CHECK(CreateShaderModule(device,&ci,nullptr,&module));return module;
 }
 void close() { DestroyCommandPool(device,pool,nullptr);DestroyDevice(device,nullptr);DestroyInstance(instance,nullptr); }
};
struct Buffer {
 Context &c;VkBuffer buffer;VkDeviceMemory memory;VkDeviceSize bytes;
 Buffer(Context &ctx,VkDeviceSize size,VkBufferUsageFlags usage):c(ctx),bytes(size) {
  VkBufferCreateInfo ci={};ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;ci.size=size;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
  CHECK(c.CreateBuffer(c.device,&ci,nullptr,&buffer));VkMemoryRequirements req;c.GetBufferMemoryRequirements(c.device,buffer,&req);
  memory=c.allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);CHECK(c.BindBufferMemory(c.device,buffer,memory,0));
  void *p=map();memset(p,0,(size_t)bytes);range(false);c.UnmapMemory(c.device,memory);
 }
 void *map() {void *p=nullptr;CHECK(c.MapMemory(c.device,memory,0,VK_WHOLE_SIZE,0,&p));return p;}
 void range(bool invalidate) {
  VkMappedMemoryRange r={};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=memory;r.size=VK_WHOLE_SIZE;
  if(invalidate)CHECK(c.InvalidateMappedMemoryRanges(c.device,1,&r));else CHECK(c.FlushMappedMemoryRanges(c.device,1,&r));
 }
 ~Buffer() {c.DestroyBuffer(c.device,buffer,nullptr);c.FreeMemory(c.device,memory,nullptr);}
};
static void fill(Context &c,unsigned iteration) {
 const uint32_t pattern=0xa740cafeu^iteration;
 Buffer b(c,65536,VK_BUFFER_USAGE_TRANSFER_DST_BIT);c.begin();c.CmdFillBuffer(c.cmd,b.buffer,0,b.bytes,pattern);
 c.host_barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);c.submit("fill");
 auto *p=static_cast<uint32_t *>(b.map());b.range(true);
 for(unsigned i=0;i<65536/4;++i)if(p[i]!=pattern)fail("fill iteration %u word %u got 0x%08x want 0x%08x",iteration,i,p[i],pattern);
 c.UnmapMemory(c.device,b.memory);printf("FUXI VK: verify fill iteration %u words 16384 PASS\n",iteration);
}
static void compute(Context &c) {
 Buffer b(c,65536,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
 VkDescriptorSetLayoutBinding binding={};binding.binding=0;binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;binding.descriptorCount=1;binding.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
 VkDescriptorSetLayoutCreateInfo li={};li.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;li.bindingCount=1;li.pBindings=&binding;
 VkDescriptorSetLayout layout;CHECK(c.CreateDescriptorSetLayout(c.device,&li,nullptr,&layout));
 VkPipelineLayoutCreateInfo pi={};pi.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;pi.setLayoutCount=1;pi.pSetLayouts=&layout;
 VkPipelineLayout pipeline_layout;CHECK(c.CreatePipelineLayout(c.device,&pi,nullptr,&pipeline_layout));
 VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
 VkDescriptorPoolCreateInfo dpi={};dpi.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;dpi.maxSets=1;dpi.poolSizeCount=1;dpi.pPoolSizes=&size;
 VkDescriptorPool pool;CHECK(c.CreateDescriptorPool(c.device,&dpi,nullptr,&pool));
 VkDescriptorSetAllocateInfo dai={};dai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;dai.descriptorPool=pool;dai.descriptorSetCount=1;dai.pSetLayouts=&layout;
 VkDescriptorSet set;CHECK(c.AllocateDescriptorSets(c.device,&dai,&set));
 VkDescriptorBufferInfo dbi={b.buffer,0,b.bytes};VkWriteDescriptorSet write={};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
 write.dstSet=set;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;write.pBufferInfo=&dbi;
 c.UpdateDescriptorSets(c.device,1,&write,0,nullptr);
 VkShaderModule module=c.shader(smoke_comp);
 VkComputePipelineCreateInfo ci={};ci.sType=VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;ci.layout=pipeline_layout;
 ci.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
 VkPipeline pipeline;CHECK(c.CreateComputePipelines(c.device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline));
 c.begin();c.CmdBindPipeline(c.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
 c.CmdBindDescriptorSets(c.cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline_layout,0,1,&set,0,nullptr);c.CmdDispatch(c.cmd,256,1,1);
 c.host_barrier(VK_ACCESS_SHADER_WRITE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);c.submit("compute");
 auto *p=static_cast<uint32_t *>(b.map());b.range(true);
 for(uint32_t i=0;i<16384;++i) {
  uint32_t expected=(i*1664525u+1013904223u)^0xa740cafeu;
  if(p[i]!=expected)fail("compute word %u got 0x%08x want 0x%08x",i,p[i],expected);
 }
 c.UnmapMemory(c.device,b.memory);puts("FUXI VK: verify compute words 16384 PASS");
 c.DestroyPipeline(c.device,pipeline,nullptr);c.DestroyShaderModule(c.device,module,nullptr);
 c.DestroyDescriptorPool(c.device,pool,nullptr);c.DestroyPipelineLayout(c.device,pipeline_layout,nullptr);c.DestroyDescriptorSetLayout(c.device,layout,nullptr);
}
static void triangle(Context &c) {
 Buffer b(c,256*256*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
 VkImageCreateInfo ii={};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_R8G8B8A8_UNORM;
 ii.extent={256,256,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
 ii.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ii.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
 VkImage image;CHECK(c.CreateImage(c.device,&ii,nullptr,&image));VkMemoryRequirements req;c.GetImageMemoryRequirements(c.device,image,&req);
 VkDeviceMemory memory=c.allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);CHECK(c.BindImageMemory(c.device,image,memory,0));
 VkImageViewCreateInfo vi={};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;
 vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkImageView view;CHECK(c.CreateImageView(c.device,&vi,nullptr,&view));
 VkAttachmentDescription attachment={};attachment.format=ii.format;attachment.samples=ii.samples;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
 attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
 attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
 VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub={};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
 VkSubpassDependency dep={};dep.srcSubpass=0;dep.dstSubpass=VK_SUBPASS_EXTERNAL;dep.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
 dep.dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;dep.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;dep.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
 VkRenderPassCreateInfo ri={};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;ri.attachmentCount=1;ri.pAttachments=&attachment;ri.subpassCount=1;ri.pSubpasses=&sub;ri.dependencyCount=1;ri.pDependencies=&dep;
 VkRenderPass render;CHECK(c.CreateRenderPass(c.device,&ri,nullptr,&render));
 VkFramebufferCreateInfo fi={};fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fi.renderPass=render;fi.attachmentCount=1;fi.pAttachments=&view;fi.width=256;fi.height=256;fi.layers=1;
 VkFramebuffer frame;CHECK(c.CreateFramebuffer(c.device,&fi,nullptr,&frame));
 VkPipelineLayoutCreateInfo li={};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;VkPipelineLayout layout;CHECK(c.CreatePipelineLayout(c.device,&li,nullptr,&layout));
 VkShaderModule vert=c.shader(smoke_vert),frag=c.shader(smoke_frag);
 VkPipelineShaderStageCreateInfo stages[2]={};
 for(unsigned i=0;i<2;++i) {stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].pName="main";}
 stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vert;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=frag;
 VkPipelineVertexInputStateCreateInfo vertex={};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
 VkPipelineInputAssemblyStateCreateInfo assembly={};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
 VkViewport viewport={0,0,256,256,0,1};VkRect2D scissor={{0,0},{256,256}};
 VkPipelineViewportStateCreateInfo vp={};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
 VkPipelineRasterizationStateCreateInfo raster={};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
 VkPipelineMultisampleStateCreateInfo ms={};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
 VkPipelineColorBlendAttachmentState blend={};blend.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo bs={};bs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;bs.attachmentCount=1;bs.pAttachments=&blend;
 VkGraphicsPipelineCreateInfo gi={};gi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gi.stageCount=2;gi.pStages=stages;gi.pVertexInputState=&vertex;gi.pInputAssemblyState=&assembly;
 gi.pViewportState=&vp;gi.pRasterizationState=&raster;gi.pMultisampleState=&ms;gi.pColorBlendState=&bs;gi.layout=layout;gi.renderPass=render;
 VkPipeline pipeline;CHECK(c.CreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&gi,nullptr,&pipeline));
 c.begin();VkClearValue clear={};clear.color.float32[3]=1;
 VkRenderPassBeginInfo bi={};bi.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;bi.renderPass=render;bi.framebuffer=frame;bi.renderArea=scissor;bi.clearValueCount=1;bi.pClearValues=&clear;
 c.CmdBeginRenderPass(c.cmd,&bi,VK_SUBPASS_CONTENTS_INLINE);c.CmdBindPipeline(c.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);c.CmdDraw(c.cmd,3,1,0,0);c.CmdEndRenderPass(c.cmd);
 VkBufferImageCopy copy={};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={256,256,1};
 c.CmdCopyImageToBuffer(c.cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,b.buffer,1,&copy);
 c.host_barrier(VK_ACCESS_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);c.submit("triangle");
 auto *p=static_cast<unsigned char *>(b.map());b.range(true);unsigned red=0,black=0;
 for(unsigned i=0;i<256*256;++i) {
  const auto *pixel=p+i*4;
  if(pixel[0]==255 && !pixel[1] && !pixel[2] && pixel[3]==255)++red;
  else if(!pixel[0] && !pixel[1] && !pixel[2] && pixel[3]==255)++black;
  else fail("triangle pixel %u,%u rgba %u,%u,%u,%u",i%256,i/256,pixel[0],pixel[1],pixel[2],pixel[3]);
 }
 const unsigned samples[][3]={{128,128,255},{96,80,255},{160,80,255},{0,0,0},{128,230,0},{64,192,0}};
 for(const auto &s:samples)if(p[(s[1]*256+s[0])*4]!=s[2])fail("triangle sample %u,%u red %u want %u",s[0],s[1],p[(s[1]*256+s[0])*4],s[2]);
 if(red<8064 || red>8320 || red+black!=65536)fail("triangle count red %u black %u want red 8192 +/-128",red,black);
 printf("FUXI VK: verify triangle inside/outside 6 red %u black %u PASS\n",red,black);c.UnmapMemory(c.device,b.memory);
 c.DestroyPipeline(c.device,pipeline,nullptr);c.DestroyShaderModule(c.device,vert,nullptr);c.DestroyShaderModule(c.device,frag,nullptr);
 c.DestroyPipelineLayout(c.device,layout,nullptr);c.DestroyFramebuffer(c.device,frame,nullptr);c.DestroyRenderPass(c.device,render,nullptr);
 c.DestroyImageView(c.device,view,nullptr);c.DestroyImage(c.device,image,nullptr);c.FreeMemory(c.device,memory,nullptr);
}
static unsigned configuration() {
 FILE *f=fopen(FUXI_SMOKE_CONF,"r");
 if(!f) {if(errno==ENOENT)return 4;fail("open config errno %d",errno);}
 char text[32]={};size_t n=fread(text,1,sizeof(text)-1,f);bool error=ferror(f);int extra=fgetc(f);fclose(f);
 if(error || extra!=EOF || !n)fail("config must contain max stage 1..4");
 char *end=nullptr;errno=0;unsigned long value=strtoul(text,&end,10);
 while(*end==' ' || *end=='\n' || *end=='\r' || *end=='\t')++end;
 if(errno || *end || memchr(text,0,n) || value<1 || value>4)fail("config must contain max stage 1..4");
 return (unsigned)value;
}
int main(int, char **) {
 setvbuf(stdout,nullptr,_IONBF,0);
 struct sigaction sa={};sa.sa_handler=expired;sigemptyset(&sa.sa_mask);sigaction(SIGALRM,&sa,nullptr);
 arm(30,"setup");unsigned maximum=configuration();
 if(geteuid()!=0)fail("requires unsandboxed root");
 if(setenv("TU_DARWIN_SERVICE","NativeAdrenoGpuPublished",1) || setenv("TU_DARWIN_OPEN_TYPE","0",1) || setenv("TU_DEBUG","nocb",1) || setenv("MESA_LOG_LEVEL","debug",1))fail("setenv errno %d",errno);
 printf("FUXI VK: smoke max-stage %u TU_DEBUG=nocb\n",maximum);
 void *library=dlopen(FUXI_SMOKE_ICD,RTLD_NOW|RTLD_LOCAL);if(!library)fail("dlopen %s",dlerror());
 auto gipa=reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(library,"vk_icdGetInstanceProcAddr"));if(!gipa)fail("missing vk_icdGetInstanceProcAddr");
 Context c;c.open(gipa);report("PASS","instance/device/graphics+compute queue");
 const char *names[]={"setup","fill","compute","triangle","reuse-reopen"};
 for(stage=1;stage<=maximum;++stage) {
  name=names[stage];arm(stage==4 ? 60 : 30,"stage");printf("FUXI VK: stage %u %s BEGIN\n",stage,name);
  if(stage==1)fill(c,0);
  if(stage==2)compute(c);
  if(stage==3)triangle(c);
  if(stage==4) {
   for(unsigned i=1;i<=16;++i)fill(c,i);
   c.close();puts("FUXI VK: destroy-device/instance PASS; reopening connection in same process");
   c=Context{};c.open(gipa);fill(c,17);compute(c);
  }
  report("PASS","verified%s",stage==4 ? " 16 recreations + device/instance reopen + fill/compute" : " readback");
 }
 stage=0;name="teardown";arm(10,"teardown");c.close();dlclose(library);alarm(0);
 puts("FUXI VK: done PASS");return 0;
}
