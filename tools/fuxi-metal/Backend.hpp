/* Original Fuxi diagnostic frontend. Vulkan headers/source remain external. */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "Translator.hpp"
#ifndef FUXI_MTL_ICD
#define FUXI_MTL_ICD "/usr/local/lib/libvulkan_freedreno.dylib"
#endif
namespace fuxi {
inline void check(VkResult r) { if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan rc "+std::to_string(r)); }
#define FUXI_VK_FUNCTIONS(X) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
 X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
 X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(CreateCommandPool) X(DestroyCommandPool) \
 X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdFillBuffer) \
 X(CmdCopyBuffer) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
 X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass) \
 X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
 X(CmdPushConstants) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdDraw) X(CmdCopyImageToBuffer) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void *contents{}; size_t length{}, allocated{}; };
struct Texture {VkImage image{};VkDeviceMemory memory{};VkImageView view{};size_t allocated{};};
struct Pipeline {VkPipeline pipeline{};VkPipelineLayout layout{};VkRenderPass render{};uint32_t pushOffset{},pushSize{},pushStages{};uint8_t pushBytes[128]{};};
struct Operation {
 Buffer *src, *dst; size_t from, to, size; uint8_t value;
 Texture *texture{};Pipeline *pipeline{};float clear[4]{};
};
class Backend {
 void *library{};
 VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
 VkPhysicalDeviceMemoryProperties memory{}; uint32_t family{};
 PFN_vkDestroyInstance DestroyInstance{};PFN_vkGetInstanceProcAddr gipa{};
#define FIELD(n) PFN_vk##n n{};
 FUXI_VK_FUNCTIONS(FIELD)
#undef FIELD
public:
 std::mutex mutex;
 bool poisoned=false;
 static constexpr size_t maxBufferLength=16*1024*1024;
 Backend() {
  setenv("TU_DARWIN_SERVICE","NativeAdrenoGpuPublished",1);
  setenv("TU_DARWIN_OPEN_TYPE","0",1);setenv("TU_DEBUG","nocb",1);setenv("MESA_LOG_LEVEL","debug",1);
  try {
   library=dlopen(FUXI_MTL_ICD,RTLD_NOW|RTLD_LOCAL);
   if(!library) throw std::runtime_error("dlopen Turnip failed");
   gipa=(PFN_vkGetInstanceProcAddr)dlsym(library,"vk_icdGetInstanceProcAddr");
   if(!gipa) throw std::runtime_error("missing ICD entrypoint");
#define INSTANCE(n) auto n=(PFN_vk##n)gipa(instance,"vk" #n); if(!n) throw std::runtime_error("missing vk" #n)
   INSTANCE(CreateInstance);
   VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="fuxi-metal";app.apiVersion=VK_API_VERSION_1_2;
   VkInstanceCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ci.pApplicationInfo=&app;
   check(CreateInstance(&ci,nullptr,&instance));
   DestroyInstance=(PFN_vkDestroyInstance)gipa(instance,"vkDestroyInstance");
   if(!DestroyInstance) throw std::runtime_error("missing vkDestroyInstance");
   INSTANCE(EnumeratePhysicalDevices);INSTANCE(GetPhysicalDeviceMemoryProperties);
   INSTANCE(GetPhysicalDeviceProperties);INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
   INSTANCE(CreateDevice);INSTANCE(GetDeviceProcAddr);
   uint32_t n=0;check(EnumeratePhysicalDevices(instance,&n,nullptr));
   if(n!=1) throw std::runtime_error("expected one physical device");
   check(EnumeratePhysicalDevices(instance,&n,&physical));
   VkPhysicalDeviceProperties props{};GetPhysicalDeviceProperties(physical,&props);
#ifndef FUXI_MTL_HOST_TEST
   if(props.apiVersion<VK_API_VERSION_1_2)throw std::runtime_error("Vulkan 1.2 required");
   if(props.vendorID!=0x5143 || props.deviceID!=0x43050a01) throw std::runtime_error("unexpected GPU");
#endif
   GetPhysicalDeviceMemoryProperties(physical,&memory);
   n=0;GetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);
   if(!n || n>64) throw std::runtime_error("invalid queue family count");
   std::vector<VkQueueFamilyProperties> families(n);GetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
   while(family<n && (!families[family].queueCount || !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))) ++family;
   if(family==n) throw std::runtime_error("no transfer queue");
   float priority=1;VkDeviceQueueCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
   VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
   check(CreateDevice(physical,&di,nullptr,&device));
#define LOAD(n) n=(PFN_vk##n)GetDeviceProcAddr(device,"vk" #n);if(!n) throw std::runtime_error("missing vk" #n);
   FUXI_VK_FUNCTIONS(LOAD)
#undef LOAD
#undef INSTANCE
   GetDeviceQueue(device,family,0,&queue);if(!queue) throw std::runtime_error("null queue");
  } catch(...) {close();throw;}
 }
 void close() {
  if(poisoned) return; // Unknown GPU ownership: leave reclamation to process death/KMD containment.
  if(device && DestroyDevice) DestroyDevice(device,nullptr);
  if(instance && DestroyInstance) DestroyInstance(instance,nullptr);
  if(library) dlclose(library);
  device={};instance={};library=nullptr;
 }
 ~Backend() {close();}
 void requireLive() {if(poisoned) throw std::runtime_error("device poisoned after submit failure");}
 Buffer *allocate(size_t length) {
  std::lock_guard<std::mutex> guard(mutex);requireLive();
  if(!length || length>maxBufferLength) throw std::runtime_error("buffer length outside 1..16 MiB");
  auto *b=new Buffer{};b->length=length;
  try {
   VkBufferCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;ci.size=length;
   ci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   check(CreateBuffer(device,&ci,nullptr,&b->buffer));
   VkMemoryRequirements req{};GetBufferMemoryRequirements(device,b->buffer,&req);b->allocated=req.size;
   uint32_t type=0;while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || !(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) ++type;
   if(type==memory.memoryTypeCount) throw std::runtime_error("no host-visible memory");
   VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=type;
   check(AllocateMemory(device,&ai,nullptr,&b->memory));check(BindBufferMemory(device,b->buffer,b->memory,0));
   check(MapMemory(device,b->memory,0,VK_WHOLE_SIZE,0,&b->contents));memset(b->contents,0,length);return b;
  } catch(...) {destroyUnlocked(b);throw;}
 }
 void destroyUnlocked(Buffer *b) {
  if(!b) return;
  if(!poisoned) {
   if(b->contents) UnmapMemory(device,b->memory);
   if(b->buffer) DestroyBuffer(device,b->buffer,nullptr);
   if(b->memory) FreeMemory(device,b->memory,nullptr);
  }
  delete b;
 }
 void destroy(Buffer *b) {std::lock_guard<std::mutex> guard(mutex);destroyUnlocked(b);}
 void destroy(Texture *t) {
  std::lock_guard<std::mutex> guard(mutex);if(!t)return;
  if(!poisoned) {if(t->view)DestroyImageView(device,t->view,nullptr);if(t->image)DestroyImage(device,t->image,nullptr);if(t->memory)FreeMemory(device,t->memory,nullptr);}delete t;
 }
 Texture *allocateTexture() {
  std::unique_lock<std::mutex> guard(mutex);requireLive();auto *t=new Texture{};
  try {
   VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=VK_FORMAT_B8G8R8A8_UNORM;
   ii.extent={256,256,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
   ii.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   check(CreateImage(device,&ii,nullptr,&t->image));VkMemoryRequirements req{};GetImageMemoryRequirements(device,t->image,&req);t->allocated=req.size;
   uint32_t type=0;while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || !(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
   if(type==memory.memoryTypeCount)throw std::runtime_error("no device-local texture memory");
   VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=type;
   check(AllocateMemory(device,&ai,nullptr,&t->memory));check(BindImageMemory(device,t->image,t->memory,0));
   VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=t->image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;
   vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(CreateImageView(device,&vi,nullptr,&t->view));return t;
  }catch(...) {guard.unlock();destroy(t);throw;}
 }
 void destroy(Pipeline *p) {
  std::lock_guard<std::mutex> guard(mutex);if(!p)return;
  if(!poisoned) {if(p->pipeline)DestroyPipeline(device,p->pipeline,nullptr);if(p->layout)DestroyPipelineLayout(device,p->layout,nullptr);if(p->render)DestroyRenderPass(device,p->render,nullptr);}delete p;
 }
 Pipeline *compilePipeline(void *v,const char *vn,void *f,const char *fn) {
  std::unique_lock<std::mutex> guard(mutex);requireLive();Translation translated{};auto &tr=Translator::get();char error[2048]{};
  auto start=std::chrono::steady_clock::now();
  int ok=tr.compile(v,vn,f,fn,(void *)gipa,(uint64_t)instance,(uint64_t)physical,&translated,error,sizeof(error));
  double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  fprintf(stderr,"FUXI MTL: translation %s %.3f ms (no cache) %s\n",ok?"PASS":"FAIL",ms,ok?"":error);
  if(!ok)throw std::runtime_error(error[0]?error:"translation failed");
  auto *p=new Pipeline{};VkShaderModule modules[2]{};
  auto cleanup=[&]{for(auto m:modules)if(m)DestroyShaderModule(device,m,nullptr);tr.free(translated.handle);};
  try {
   VkAttachmentDescription attachment{};attachment.format=VK_FORMAT_B8G8R8A8_UNORM;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
   attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
   attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
   attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   VkAttachmentReference ref{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
   VkSubpassDependency deps[2]{};deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;
   deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   deps[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
   deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
   deps[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;deps[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
   VkRenderPassCreateInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;ri.attachmentCount=1;ri.pAttachments=&attachment;ri.subpassCount=1;ri.pSubpasses=&sub;ri.dependencyCount=2;ri.pDependencies=deps;
   check(CreateRenderPass(device,&ri,nullptr,&p->render));
   if(translated.pushSize!=4 || translated.pushOffset>124 || translated.pushStages!=VK_SHADER_STAGE_FRAGMENT_BIT)throw std::runtime_error("invalid translator push constant range");
   p->pushOffset=translated.pushOffset;p->pushSize=translated.pushSize;p->pushStages=translated.pushStages;memcpy(p->pushBytes,translated.pushBytes,p->pushSize);
   VkPushConstantRange range{p->pushStages,p->pushOffset,p->pushSize};
   VkPipelineLayoutCreateInfo li{};li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.pushConstantRangeCount=1;li.pPushConstantRanges=&range;check(CreatePipelineLayout(device,&li,nullptr,&p->layout));
   const uint32_t *words[]={translated.vertex,translated.fragment};size_t counts[]={translated.vertexCount,translated.fragmentCount};
   for(unsigned i=0;i<2;++i) {
    if(!words[i] || counts[i]<5 || counts[i]>1024*1024 || words[i][0]!=0x07230203)throw std::runtime_error("invalid translator SPIR-V");
    VkShaderModuleCreateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;mi.codeSize=counts[i]*4;mi.pCode=words[i];check(CreateShaderModule(device,&mi,nullptr,&modules[i]));
   }
   VkPipelineShaderStageCreateInfo stages[2]{};
   for(unsigned i=0;i<2;++i) {stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].module=modules[i];}
   stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].pName=translated.vertexEntry;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].pName=translated.fragmentEntry;
   VkPipelineVertexInputStateCreateInfo vertex{};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   // Metal's viewport has a top-left origin. Vulkan 1.2 supports negative height.
   VkViewport viewport{0,256,256,-256,0,1};VkRect2D scissor{{0,0},{256,256}};
   VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
   VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
   VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=15;VkPipelineColorBlendStateCreateInfo bs{};bs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;bs.attachmentCount=1;bs.pAttachments=&blend;
   VkGraphicsPipelineCreateInfo gi{};gi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gi.stageCount=2;gi.pStages=stages;gi.pVertexInputState=&vertex;gi.pInputAssemblyState=&assembly;
   gi.pViewportState=&vp;gi.pRasterizationState=&raster;gi.pMultisampleState=&ms;gi.pColorBlendState=&bs;gi.layout=p->layout;gi.renderPass=p->render;
   check(CreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gi,nullptr,&p->pipeline));cleanup();return p;
  }catch(...) {cleanup();guard.unlock();destroy(p);throw;}
 }
 void submit(const std::vector<Operation>& ops) {
  std::lock_guard<std::mutex> guard(mutex);requireLive();
  VkCommandPool pool{};VkFence fence{};bool submitted=false;std::vector<VkFramebuffer> frames;
  auto cleanup=[&] {for(auto frame:frames)DestroyFramebuffer(device,frame,nullptr);if(fence) DestroyFence(device,fence,nullptr);if(pool) DestroyCommandPool(device,pool,nullptr);};
  try {
   std::vector<Buffer *> buffers;
   for(const auto &op:ops) for(auto *b:{op.src,op.dst}) if(b && std::find(buffers.begin(),buffers.end(),b)==buffers.end()) buffers.push_back(b);
   for(auto *b:buffers) {VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=b->memory;r.size=VK_WHOLE_SIZE;check(FlushMappedMemoryRanges(device,1,&r));}
   VkCommandPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.queueFamilyIndex=family;check(CreateCommandPool(device,&pi,nullptr,&pool));
   VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ai.commandPool=pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
   VkCommandBuffer cmd{};check(AllocateCommandBuffers(device,&ai,&cmd));
   VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(BeginCommandBuffer(cmd,&bi));
   VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
   CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
   for(const auto &op:ops) {
    if(op.texture && op.pipeline) {
     VkFramebufferCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fi.renderPass=op.pipeline->render;
     fi.attachmentCount=1;fi.pAttachments=&op.texture->view;fi.width=256;fi.height=256;fi.layers=1;VkFramebuffer frame{};
     check(CreateFramebuffer(device,&fi,nullptr,&frame));frames.push_back(frame);
     VkClearValue clear{};memcpy(clear.color.float32,op.clear,sizeof(op.clear));
     VkRenderPassBeginInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;ri.renderPass=op.pipeline->render;ri.framebuffer=frame;
     ri.renderArea={{0,0},{256,256}};ri.clearValueCount=1;ri.pClearValues=&clear;
     CmdBeginRenderPass(cmd,&ri,VK_SUBPASS_CONTENTS_INLINE);CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,op.pipeline->pipeline);
     CmdPushConstants(cmd,op.pipeline->layout,op.pipeline->pushStages,op.pipeline->pushOffset,op.pipeline->pushSize,op.pipeline->pushBytes);
     CmdDraw(cmd,3,1,0,0);CmdEndRenderPass(cmd);
    } else if(op.texture) {
     VkBufferImageCopy region{};region.bufferOffset=op.to;region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={256,256,1};
     CmdCopyImageToBuffer(cmd,op.texture->image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,op.dst->buffer,1,&region);
    } else if(op.src) {VkBufferCopy region{op.from,op.to,op.size};CmdCopyBuffer(cmd,op.src->buffer,op.dst->buffer,1,&region);}
    else CmdFillBuffer(cmd,op.dst->buffer,op.to,op.size,uint32_t(op.value)*0x01010101u);
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
    CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
   }
   barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
   CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
   check(EndCommandBuffer(cmd));VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;check(CreateFence(device,&fi,nullptr,&fence));
   VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&cmd;
   // Any failure after attempting submission poisons ownership and avoids unsafe teardown.
   submitted=true;check(QueueSubmit(queue,1,&si,fence));check(WaitForFences(device,1,&fence,VK_TRUE,2000000000ULL));
   submitted=false;
   for(auto *b:buffers) {VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=b->memory;r.size=VK_WHOLE_SIZE;check(InvalidateMappedMemoryRanges(device,1,&r));}
   cleanup();
  } catch(...) {if(submitted) poisoned=true;else cleanup();throw;}
 }
};
}
