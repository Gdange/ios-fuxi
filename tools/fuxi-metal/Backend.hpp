/* Original Fuxi transfer-only frontend. Vulkan headers/source remain external. */
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
 X(CmdCopyBuffer) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void *contents{}; size_t length{}, allocated{}; };
struct Operation { Buffer *src, *dst; size_t from, to, size; uint8_t value; };
class Backend {
 void *library{};
 VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
 VkPhysicalDeviceMemoryProperties memory{}; uint32_t family{};
 PFN_vkDestroyInstance DestroyInstance{};
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
   auto gipa=(PFN_vkGetInstanceProcAddr)dlsym(library,"vk_icdGetInstanceProcAddr");
   if(!gipa) throw std::runtime_error("missing ICD entrypoint");
#define INSTANCE(n) auto n=(PFN_vk##n)gipa(instance,"vk" #n); if(!n) throw std::runtime_error("missing vk" #n)
   INSTANCE(CreateInstance);
   VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="fuxi-metal";app.apiVersion=VK_API_VERSION_1_1;
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
   if(props.vendorID!=0x5143 || props.deviceID!=0x43050a01) throw std::runtime_error("unexpected GPU");
#endif
   GetPhysicalDeviceMemoryProperties(physical,&memory);
   n=0;GetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);
   if(!n || n>64) throw std::runtime_error("invalid queue family count");
   std::vector<VkQueueFamilyProperties> families(n);GetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
   while(family<n && (!families[family].queueCount || !(families[family].queueFlags&(VK_QUEUE_TRANSFER_BIT|VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)))) ++family;
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
 void submit(const std::vector<Operation>& ops) {
  std::lock_guard<std::mutex> guard(mutex);requireLive();
  VkCommandPool pool{};VkFence fence{};bool submitted=false;
  auto cleanup=[&] {if(fence) DestroyFence(device,fence,nullptr);if(pool) DestroyCommandPool(device,pool,nullptr);};
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
    if(op.src) {VkBufferCopy region{op.from,op.to,op.size};CmdCopyBuffer(cmd,op.src->buffer,op.dst->buffer,1,&region);}
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
