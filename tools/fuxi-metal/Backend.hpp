/* Original Fuxi diagnostic frontend. Vulkan headers/source remain external. */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <mutex>
#include <atomic>
#include <chrono>
#include "ResourceLimits.hpp"
#include "TextureLevels.hpp"
#include "ObjectCache.hpp"
#include <functional>
#include <stdexcept>
#include <vector>
#include "Translator.hpp"
#include "PipelineCache.hpp"
#include "BackboardGate.hpp"
#include "FramebufferFetch.hpp"
#include "VertexProfile.hpp"
#include "../../xnu/fuxi-adreno/NativeAdrenoABI.h"
static_assert(NativeAdrenoAbiVersion==2,"FuxiMetal HostImport requires NativeAdreno ABI 2");
#ifndef FUXI_MTL_ICD
#define FUXI_MTL_ICD "/usr/local/lib/libvulkan_freedreno.dylib"
#endif
namespace fuxi {
inline bool hostRangesOverlap(const void *a,size_t an,const void *b,size_t bn){
 if(!a || !b || !an || !bn)return false;auto x=(uintptr_t)a,y=(uintptr_t)b;return x<=y?y-x<an:x-y<bn;
}
// Live Vulkan memory objects of this process: each is one kernel BO, a per-client-limited resource.
inline std::atomic<int64_t> liveMemoryObjects{0},liveMemoryBytes{0};
// The line names the failing call; the counters tell a BO quota refusal from real exhaustion.
inline void check(VkResult r,int line=__builtin_LINE()) {
 if(r!=VK_SUCCESS)backboardGate.fault();
 if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan rc "+std::to_string(r)+" at Backend.hpp:"+std::to_string(line)+" (memory objects "+std::to_string(liveMemoryObjects.load())+", "+std::to_string(liveMemoryBytes.load()>>10)+" KiB)");
}
#define FUXI_VK_FUNCTIONS(X) \
 X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) X(CmdResetQueryPool) X(CmdWriteTimestamp) \
 X(DestroyDevice) X(GetDeviceQueue) X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) \
 X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(UnmapMemory) \
 X(FlushMappedMemoryRanges) X(InvalidateMappedMemoryRanges) X(CreateCommandPool) X(DestroyCommandPool) \
 X(ResetCommandPool) X(ResetFences) X(ResetDescriptorPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) X(CmdFillBuffer) \
 X(CmdCopyBuffer) X(CmdClearColorImage) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
 X(GetImageSubresourceLayout) X(CmdCopyBufferToImage) X(CmdBlitImage) X(CmdCopyImage) X(CreateSampler) X(DestroySampler) \
 X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) \
 X(AllocateDescriptorSets) X(UpdateDescriptorSets) X(CmdBindDescriptorSets) X(CreateImageView) X(DestroyImageView) X(CreateRenderPass) X(DestroyRenderPass) \
 X(CreateFramebuffer) X(DestroyFramebuffer) X(CreateShaderModule) X(DestroyShaderModule) \
 X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateGraphicsPipelines) X(DestroyPipeline) \
 X(CreatePipelineCache) X(DestroyPipelineCache) X(GetPipelineCacheData) \
 X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdDrawIndexed) X(CmdSetViewport) X(CmdSetScissor) X(CmdPushConstants) X(CmdBeginRenderPass) X(CmdEndRenderPass) X(CmdBindPipeline) X(CmdDraw) X(CmdCopyImageToBuffer) X(CmdPipelineBarrier) X(CreateFence) X(DestroyFence) X(QueueSubmit) X(WaitForFences)
struct Buffer { VkBuffer buffer{}; VkDeviceMemory memory{}; void *contents{}; size_t length{}, allocated{},charged{},memoryBytes{};uint32_t memoryType{};bool imported=false; };
struct Texture : TextureLevels {VkImage image{};VkDeviceMemory memory{};VkImageView view{};size_t allocated{},charged{},memoryBytes{};
 uint32_t memoryType{};VkImageUsageFlags usage{};uint32_t format=80;VkFormat vkFormat=VK_FORMAT_B8G8R8A8_UNORM;
 VkImageTiling tiling=VK_IMAGE_TILING_OPTIMAL;VkImageLayout layout=VK_IMAGE_LAYOUT_UNDEFINED;void *mapped{};bool imported=false,borrowedMemory=false;
 std::vector<VkImageView> attachmentViews;bool canGenerate=false;};
struct Sampler {VkSampler sampler{};uint32_t filters=1;float lodMin=0,lodMax=0;};
struct Pipeline {size_t references=1,retainedBytes=0;void *translation{};CompileOptions options{};uint32_t fetchMask{};bool orderedColorAccess=false;std::vector<std::pair<uint32_t,VkRenderPass>> renders;VkPipeline pipeline{},stripPipeline{};VkPipelineLayout layout{};VkRenderPass render{};uint32_t pushOffset{},pushSize{},pushStages{};uint8_t pushBytes[128]{};VkDescriptorSetLayout setLayout{};VkSampler bakedSamplers[16]{};std::vector<Descriptor> descriptors;std::vector<Buffer *> constantBlobs;std::vector<std::array<uint8_t,32>> blobHashes;std::vector<std::pair<std::string,Pipeline *>> variants;};
struct BufferBind {Buffer *buffer{};uint64_t offset{};bool bytes=false;};
struct ColorTarget {Texture *texture{};uint32_t level{},load=2,store=1;float clear[4]{};};
struct Operation {
 Buffer *src, *dst; size_t from, to, size; uint8_t value;
 Texture *texture{};Pipeline *pipeline{};float clear[4]{};
 Texture *sourceTexture{};Sampler *sampler{};uint32_t vertices=3;bool upload=false,clearOnly=false;
 uint32_t srcX{},srcY{},dstX{},dstY{},width{},height{};size_t rowBytes{};
 Texture *samples[resourceSlots]{};Sampler *samplers[resourceSlots]{};BufferBind buffers[2][resourceSlots]{};Buffer *indices{};uint64_t indexOffset{};uint32_t first{},strip{},load=2,store=1;
 VkViewport viewport{0,256,256,-256,0,1};VkRect2D scissor{{0,0},{256,256}};
 // Metal renderTargetWidth/Height: the pass's top-left render area; 0 means the whole target.
 uint32_t areaWidth{},areaHeight{};
 uint32_t srcLevel{},dstLevel{},sampleBase[resourceSlots]{},sampleCount[resourceSlots]{};bool generateMips=false;uint32_t mipBase{},mipCount{};
 std::string diagnostic;// texture/dstLevel also name the first present target when slot 0 is absent.
 uint32_t colorCount=1,colorMask=1;ColorTarget colors[7]{};
 ColorTarget color(unsigned i) const {if(!(colorMask&(1u<<i)))return {};if(i)return colors[i-1];ColorTarget c{texture,dstLevel,load,store,{}};memcpy(c.clear,clear,sizeof(clear));return c;}
 void setColor(unsigned i,const ColorTarget &c){if(i){colors[i-1]=c;return;}texture=c.texture;dstLevel=c.level;load=c.load;store=c.store;memcpy(clear,c.clear,sizeof(clear));}

};
class Backend {
 void *library{};
 VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
 bool independentBlend=false;uint32_t maxColorAttachments=1;
 PipelineCache pipelineCache;
 ObjectCache<Pipeline> objects;ResourceLimits limits;
 // One synchronous lane, protected by mutex; never reset while GPU ownership is uncertain.
 VkCommandPool commandPool{};VkCommandBuffer command{};VkFence submitFence{};VkQueryPool queryPool{};
 VkDescriptorPool descriptorPool{};uint32_t descriptorSets{};std::vector<VkDescriptorPoolSize> descriptorCapacity;
 struct FrameEntry {std::array<Texture *,8> textures;VkRenderPass render;std::array<uint32_t,8> levels;VkFramebuffer frame;};
 struct ViewEntry {Texture *texture;VkFormat format;std::array<uint32_t,4> components;uint32_t base,count;VkImageView view;};
 std::vector<FrameEntry> frameCache;std::vector<ViewEntry> viewCache;
 static constexpr size_t frameCacheLimit=128,viewCacheLimit=128;
 void clearSubmitObjects() {
  if(descriptorPool)DestroyDescriptorPool(device,descriptorPool,nullptr);
  if(queryPool)DestroyQueryPool(device,queryPool,nullptr);
  if(submitFence)DestroyFence(device,submitFence,nullptr);
  if(commandPool)DestroyCommandPool(device,commandPool,nullptr);
  descriptorPool={};queryPool={};submitFence={};commandPool={};command={};descriptorSets=0;descriptorCapacity.clear();
 }
 void clearFrames(Texture *texture=nullptr,VkRenderPass render=VK_NULL_HANDLE) {
  for(auto i=frameCache.begin();i!=frameCache.end();) {
   if((!texture && !render) || (texture && std::find(i->textures.begin(),i->textures.end(),texture)!=i->textures.end()) || (render && i->render==render)){DestroyFramebuffer(device,i->frame,nullptr);i=frameCache.erase(i);}else ++i;
  }
 }
 void clearViews(Texture *texture=nullptr) {
  for(auto i=viewCache.begin();i!=viewCache.end();) {
   if(!texture || i->texture==texture){DestroyImageView(device,i->view,nullptr);i=viewCache.erase(i);}else ++i;
  }
 }
 bool coherent(uint32_t type) const {return memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;}

 uint32_t timestampBits=0;float timestampPeriod=0;
 VkPhysicalDeviceMemoryProperties memory{}; uint32_t family{};
 bool orderedColorAccess=false;bool hostImport=false;VkDeviceSize hostAlignment{};VkFormatFeatureFlags linearFeatures{},optimalFeatures{};
 PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties{};
 PFN_vkGetPhysicalDeviceExternalBufferProperties GetPhysicalDeviceExternalBufferProperties{};
 PFN_vkGetMemoryHostPointerPropertiesEXT GetMemoryHostPointerPropertiesEXT{};
 PFN_vkGetPhysicalDeviceImageFormatProperties2 GetPhysicalDeviceImageFormatProperties2{};
 PFN_vkDestroyInstance DestroyInstance{};PFN_vkGetInstanceProcAddr gipa{};
#define FIELD(n) PFN_vk##n n{};
 FUXI_VK_FUNCTIONS(FIELD)
#undef FIELD
public:
 std::mutex mutex;
 bool poisoned=false;
 static constexpr size_t maxBufferLength=ResourceLimits::single;
 static inline std::atomic<uint64_t> pipelineHits{0},pipelineMisses{0},gpuNanos{0},gpuSamples{0};
 static inline auto &pipelineCacheLoadResult=PipelineCache::loadResult,&pipelineCacheLoadBytes=PipelineCache::loadBytes;
 static inline auto &pipelineCacheSaveResult=PipelineCache::saveResult,&pipelineCacheSaveBytes=PipelineCache::saveBytes;
 static inline auto &pipelineCacheSaveCount=PipelineCache::saveCount,&pipelineCacheSaveFailures=PipelineCache::saveFailures;
 // CPU cost of submit(): entry to QueueSubmit (incl. the lock), the fence wait, and retirement.
 static inline std::atomic<uint64_t> submits{0},submitPrepareNanos{0},submitWaitNanos{0},submitRetireNanos{0};
 static inline std::atomic<uint64_t> submitObjectsNanos{0},submitDescriptorsNanos{0},submitRecordNanos{0},shadowCopyNanos{0},submitFlushNanos{0},submitInvalidateNanos{0};
 static inline std::atomic<uint64_t> flushCalls{0},invalidateCalls{0},emptySubmits{0},submitObjectCreates{0},submitPoolCreates{0},submitFrameCreates{0},submitViewCreates{0};
 struct ReuseCounts {uint64_t commandPools{},commandBuffers{},fences{},queries{},descriptors{},frames{},views{};} reuseCounts;
#ifdef FUXI_MTL_HOST_TEST
 // Test-only reference mode and deterministic recording failure; no production environment switches.
 bool reuseSubmitObjects=true,failBeforeQueue=false,failQueueAttempt=false;
 size_t cachedFrames() const{return frameCache.size();}
 size_t cachedViews() const{return viewCache.size();}
 void dropPipelineCacheForTest(){std::lock_guard<std::mutex> guard(mutex);requireLive();objects.clear([&](Pipeline *p){destroyPipelineUnlocked(p);});}
 void discardReuseForTest(){std::lock_guard<std::mutex> guard(mutex);requireLive();clearSubmitObjects();clearFrames();clearViews();}
#endif
 uint32_t maxTextureDimension() const{return limits.dimension;}
 size_t resourceBytes() const{return limits.used;}
 bool supportsTimestamps() const{return timestampBits && timestampPeriod>0;}
 explicit Backend(const char *client="unknown") {
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
   INSTANCE(GetPhysicalDeviceProperties);INSTANCE(GetPhysicalDeviceQueueFamilyProperties);GetPhysicalDeviceFormatProperties=(PFN_vkGetPhysicalDeviceFormatProperties)gipa(instance,"vkGetPhysicalDeviceFormatProperties");if(!GetPhysicalDeviceFormatProperties)throw std::runtime_error("missing format query");
   INSTANCE(GetPhysicalDeviceFeatures2);INSTANCE(CreateDevice);INSTANCE(GetDeviceProcAddr);INSTANCE(EnumerateDeviceExtensionProperties);INSTANCE(GetPhysicalDeviceProperties2);
   GetPhysicalDeviceExternalBufferProperties=(PFN_vkGetPhysicalDeviceExternalBufferProperties)gipa(instance,"vkGetPhysicalDeviceExternalBufferProperties");
   GetPhysicalDeviceImageFormatProperties2=(PFN_vkGetPhysicalDeviceImageFormatProperties2)gipa(instance,"vkGetPhysicalDeviceImageFormatProperties2");
   uint32_t n=0;check(EnumeratePhysicalDevices(instance,&n,nullptr));
   if(n!=1) throw std::runtime_error("expected one physical device");
   check(EnumeratePhysicalDevices(instance,&n,&physical));
   VkPhysicalDeviceProperties props{};GetPhysicalDeviceProperties(physical,&props);
#ifndef FUXI_MTL_HOST_TEST
   if(props.apiVersion<VK_API_VERSION_1_2)throw std::runtime_error("Vulkan 1.2 required");
   if(props.vendorID!=0x5143 || props.deviceID!=0x43050a01) throw std::runtime_error("unexpected GPU");
#endif
#ifdef FUXI_MTL_CA_OFFLINE
   if(props.deviceType!=VK_PHYSICAL_DEVICE_TYPE_CPU || strncmp(props.deviceName,"llvmpipe",8))throw std::runtime_error("offline qualifier requires lavapipe CPU device");
#endif
   limits=ResourceLimits(props.limits.maxImageDimension2D);timestampPeriod=props.limits.timestampPeriod;
   fprintf(stderr,"FUXI MTL: texture limits dimension=%u budget=%zu MiB\n",limits.dimension,ResourceLimits::budget/(1024*1024));
   GetPhysicalDeviceMemoryProperties(physical,&memory);
   VkFormatProperties format{};GetPhysicalDeviceFormatProperties(physical,VK_FORMAT_B8G8R8A8_UNORM,&format);
   linearFeatures=format.linearTilingFeatures;optimalFeatures=format.optimalTilingFeatures;
   n=0;GetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);
   if(!n || n>64) throw std::runtime_error("invalid queue family count");
   std::vector<VkQueueFamilyProperties> families(n);GetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
   while(family<n && (!families[family].queueCount || !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))) ++family;
   if(family==n) throw std::runtime_error("no transfer queue");
   timestampBits=families[family].timestampValidBits;
   float priority=1;VkDeviceQueueCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
   VkDeviceCreateInfo di{};di.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;
   uint32_t extCount=0;check(EnumerateDeviceExtensionProperties(physical,nullptr,&extCount,nullptr));
   if(extCount>4096)throw std::runtime_error("extension count");
   std::vector<VkExtensionProperties> extensions(extCount);check(EnumerateDeviceExtensionProperties(physical,nullptr,&extCount,extensions.data()));
   for(const auto &ext:extensions)if(!strcmp(ext.extensionName,VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME))hostImport=true;
   std::vector<const char *> enabledExtensions;
   for(const auto &ext:extensions)if(!strcmp(ext.extensionName,VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME))orderedColorAccess=true;
   VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT order{};
   order.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT;
   if(orderedColorAccess) {
    VkPhysicalDeviceFeatures2 supported{};supported.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;supported.pNext=&order;
    GetPhysicalDeviceFeatures2(physical,&supported);orderedColorAccess=order.rasterizationOrderColorAttachmentAccess;
    order.rasterizationOrderDepthAttachmentAccess=order.rasterizationOrderStencilAttachmentAccess=VK_FALSE;
    if(orderedColorAccess){enabledExtensions.push_back(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);di.pNext=&order;}
   }
   VkPhysicalDeviceFeatures2 core{};core.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;GetPhysicalDeviceFeatures2(physical,&core);
   VkPhysicalDeviceFeatures enables{};enables.independentBlend=core.features.independentBlend;di.pEnabledFeatures=&enables;
   independentBlend=enables.independentBlend;maxColorAttachments=std::min(8u,props.limits.maxColorAttachments);
   fprintf(stderr,"FUXI MTL: ordered framebuffer fetch %s (input attachments)\n",orderedColorAccess?"enabled":"unavailable");
   if(hostImport) {
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{};hp.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT;
    VkPhysicalDeviceProperties2 p2{};p2.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;p2.pNext=&hp;GetPhysicalDeviceProperties2(physical,&p2);
    hostAlignment=hp.minImportedHostPointerAlignment;
#ifndef FUXI_MTL_HOST_TEST
    if(hostAlignment!=NativeAdrenoAbiHostPageBytes)throw std::runtime_error("NativeAdreno ABI 2 HostImport requires 16K pages");
#endif
    enabledExtensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
   }
   di.enabledExtensionCount=enabledExtensions.size();di.ppEnabledExtensionNames=enabledExtensions.data();
   check(CreateDevice(physical,&di,nullptr,&device));
   GetMemoryHostPointerPropertiesEXT=(PFN_vkGetMemoryHostPointerPropertiesEXT)GetDeviceProcAddr(device,"vkGetMemoryHostPointerPropertiesEXT");
#define LOAD(n) n=(PFN_vk##n)GetDeviceProcAddr(device,"vk" #n);if(!n) throw std::runtime_error("missing vk" #n);
   FUXI_VK_FUNCTIONS(LOAD)
#undef LOAD
#undef INSTANCE
   VkPhysicalDeviceDriverProperties driver{};driver.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
   VkPhysicalDeviceIDProperties id{};id.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;driver.pNext=&id;
   VkPhysicalDeviceProperties2 identity{};identity.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;identity.pNext=&driver;
   GetPhysicalDeviceProperties2(physical,&identity);
   uint8_t translator[32]{};bool identified=Translator::cacheIdentity(translator);
   pipelineCache.open(device,CreatePipelineCache,DestroyPipelineCache,GetPipelineCacheData,props,driver,id,identified?translator:nullptr,client);
   requireLive();
   GetDeviceQueue(device,family,0,&queue);if(!queue) throw std::runtime_error("null queue");
  } catch(...) {close();throw;}
 }
 void close() {
  if(poisoned) return; // Unknown GPU ownership: leave reclamation to process death/KMD containment.
  clearSubmitObjects();clearFrames();clearViews();
  objects.clear([&](Pipeline *p){destroyPipelineUnlocked(p);});
  pipelineCache.close();
  if(device && DestroyDevice) DestroyDevice(device,nullptr);
  if(instance && DestroyInstance) DestroyInstance(instance,nullptr);
  if(library) dlclose(library);
  device={};instance={};library=nullptr;
 }
 ~Backend() {close();}
 void requireLive() {if(poisoned || backboardGate.blocked()) throw std::runtime_error("device poisoned or backboardd failure latched");}
 Buffer *allocate(size_t length) {
  std::lock_guard<std::mutex> guard(mutex);requireLive();
  return allocateUnlocked(length);
 }
 Buffer *allocateUnlocked(size_t length) {
  if(!length || length>maxBufferLength) throw std::runtime_error("buffer length outside resource limit");
  auto *b=new Buffer{};b->length=length;
  try {
   VkBufferCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;ci.size=(length+15)&~size_t(15);
   ci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   check(CreateBuffer(device,&ci,nullptr,&b->buffer));
   VkMemoryRequirements req{};GetBufferMemoryRequirements(device,b->buffer,&req);b->allocated=req.size;
   uint32_t type=0;while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || !(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) ++type;
   if(type==memory.memoryTypeCount) throw std::runtime_error("no host-visible memory");
   VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=type;b->memoryType=type;
   limits.reserve(ai.allocationSize);b->charged=ai.allocationSize;
   check(AllocateMemory(device,&ai,nullptr,&b->memory));++liveMemoryObjects;liveMemoryBytes+=ai.allocationSize;b->memoryBytes=ai.allocationSize;check(BindBufferMemory(device,b->buffer,b->memory,0));
   check(MapMemory(device,b->memory,0,VK_WHOLE_SIZE,0,&b->contents));memset(b->contents,0,(length+15)&~size_t(15));return b;
  } catch(...) {destroyUnlocked(b);throw;}
 }
 // NativeMetal's no-copy buffer/linear-image design, using Fuxi HostImport's
 // page contract. Reject layouts that Vulkan cannot bind to the same pages.
 Buffer *importBuffer(void *base,size_t bytes) {
  std::lock_guard<std::mutex> guard(mutex);requireLive();
  if(!supportsHostImport() || !base || !bytes || bytes>maxBufferLength || uintptr_t(base)%hostAlignment || bytes%hostAlignment)throw std::runtime_error("no-copy buffer host alignment/length");
  auto *b=new Buffer{};b->length=bytes;b->imported=true;
  try {
   VkExternalMemoryBufferCreateInfo ext{};ext.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
   VkBufferCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;ci.pNext=&ext;ci.size=bytes;
   ci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   if(!GetPhysicalDeviceExternalBufferProperties)throw std::runtime_error("missing external buffer query");
   VkPhysicalDeviceExternalBufferInfo info{};info.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;info.usage=ci.usage;info.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
   VkExternalBufferProperties properties{};properties.sType=VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;GetPhysicalDeviceExternalBufferProperties(physical,&info,&properties);
   auto externalFeatures=properties.externalMemoryProperties.externalMemoryFeatures;
   if(!(externalFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || (externalFeatures&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) || !(properties.externalMemoryProperties.compatibleHandleTypes&info.handleType))throw std::runtime_error("no-copy buffer external import unavailable");
   check(CreateBuffer(device,&ci,nullptr,&b->buffer));VkMemoryRequirements req{};GetBufferMemoryRequirements(device,b->buffer,&req);
   if(req.size>bytes)throw std::runtime_error("no-copy buffer allocation size");
   VkMemoryHostPointerPropertiesEXT hp{};hp.sType=VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;check(GetMemoryHostPointerPropertiesEXT(device,VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,base,&hp));
   uint32_t type=0;while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&hp.memoryTypeBits&(1u<<type)) || !(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))++type;
   if(type==memory.memoryTypeCount)throw std::runtime_error("no-copy buffer memory type");
   VkImportMemoryHostPointerInfoEXT hi{};hi.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;hi.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;hi.pHostPointer=base;
   VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.pNext=&hi;ai.allocationSize=bytes;ai.memoryTypeIndex=type;
   limits.reserve(ai.allocationSize);b->charged=ai.allocationSize;
   check(AllocateMemory(device,&ai,nullptr,&b->memory));++liveMemoryObjects;liveMemoryBytes+=ai.allocationSize;b->memoryBytes=ai.allocationSize;b->allocated=bytes;b->memoryType=type;check(BindBufferMemory(device,b->buffer,b->memory,0));
   check(MapMemory(device,b->memory,0,VK_WHOLE_SIZE,0,&b->contents));return b;
  }catch(...){destroyUnlocked(b);throw;}
 }
 Texture *linearTexture(Buffer *b,uint32_t format,uint32_t usage,uint32_t width,uint32_t height,size_t offset,size_t row) {
  std::unique_lock<std::mutex> guard(mutex);requireLive();
  if(!b || !limits.rows(width,height,texelBytes(format),row,offset,b->length) || !usage || usage&~5u || format==81 || (format==1 && (usage&4)))throw std::runtime_error("linear texture descriptor/extent/stride");
  auto *t=new Texture{};t->format=format;t->vkFormat=pixelFormat(format);t->width=width;t->height=height;t->tiling=VK_IMAGE_TILING_LINEAR;
  try {
   VkImageCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ci.imageType=VK_IMAGE_TYPE_2D;ci.format=t->vkFormat;
   ci.flags=VK_IMAGE_CREATE_ALIAS_BIT;ci.extent={width,height,1};ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_LINEAR;ci.initialLayout=VK_IMAGE_LAYOUT_PREINITIALIZED;
   ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|(usage&1?VK_IMAGE_USAGE_SAMPLED_BIT:0)|(usage&4?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0)|(orderedColorAccess && (usage&4)?VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT:0);
   VkExternalMemoryImageCreateInfo external{};external.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;external.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
   VkFormatProperties formatProperties{};GetPhysicalDeviceFormatProperties(physical,ci.format,&formatProperties);
   auto required=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT|(usage&1?VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT:0)|(usage&4?VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT:0);
   if((formatProperties.linearTilingFeatures&required)!=(uint32_t)required)throw std::runtime_error("linear texture format features unavailable");
   if(b->imported){
    ci.pNext=&external;
    VkPhysicalDeviceExternalImageFormatInfo extInfo{};extInfo.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;extInfo.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkPhysicalDeviceImageFormatInfo2 info{};info.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;info.pNext=&extInfo;info.format=ci.format;info.type=ci.imageType;info.tiling=ci.tiling;info.usage=ci.usage;info.flags=ci.flags;
    VkExternalImageFormatProperties extProps{};extProps.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;VkImageFormatProperties2 props{};props.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;props.pNext=&extProps;
    check(GetPhysicalDeviceImageFormatProperties2(physical,&info,&props));auto flags=extProps.externalMemoryProperties.externalMemoryFeatures;
    if(!(flags&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || (flags&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) || !(extProps.externalMemoryProperties.compatibleHandleTypes&extInfo.handleType))throw std::runtime_error("linear texture external import unavailable");
   }
   check(CreateImage(device,&ci,nullptr,&t->image));VkMemoryRequirements req{};GetImageMemoryRequirements(device,t->image,&req);
   VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};GetImageSubresourceLayout(device,t->image,&sub,&layout);
   if(offset%req.alignment || !(req.memoryTypeBits&(1u<<b->memoryType)) || offset>b->allocated || req.size>b->allocated-offset || layout.offset || layout.rowPitch!=row)throw std::runtime_error("linear texture Vulkan memory/layout mismatch");
   check(BindImageMemory(device,t->image,b->memory,offset));t->memory=b->memory;t->memoryType=b->memoryType;t->borrowedMemory=true;t->allocated=req.size;
   t->mapped=(uint8_t *)b->contents+offset;t->layout=ci.initialLayout;t->defined=true;t->usage=ci.usage;
   VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=t->image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=t->vkFormat;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
   check(CreateImageView(device,&vi,nullptr,&t->view));return t;
  }catch(...){guard.unlock();destroy(t);throw;}
 }
 void destroyUnlocked(Buffer *b) {
  if(!b) return;
  if(!poisoned) {
   if(b->contents) UnmapMemory(device,b->memory);
   if(b->buffer) DestroyBuffer(device,b->buffer,nullptr);
   if(b->memory) {FreeMemory(device,b->memory,nullptr);--liveMemoryObjects;liveMemoryBytes-=b->memoryBytes;}
  }
  limits.release(b->charged);delete b;
 }
 void destroy(Buffer *b) {std::lock_guard<std::mutex> guard(mutex);destroyUnlocked(b);}
 bool destroy(Texture *t) {
  std::lock_guard<std::mutex> guard(mutex);if(!t)return !poisoned;
  if(!poisoned) {clearFrames(t);clearViews(t);if(t->mapped && !t->borrowedMemory)UnmapMemory(device,t->memory);for(auto v:t->attachmentViews)if(v)DestroyImageView(device,v,nullptr);if(t->view)DestroyImageView(device,t->view,nullptr);if(t->image)DestroyImage(device,t->image,nullptr);if(t->memory && !t->borrowedMemory){FreeMemory(device,t->memory,nullptr);--liveMemoryObjects;liveMemoryBytes-=t->memoryBytes;}}limits.release(t->charged);delete t;return !poisoned;
 }
 bool supportsHostImport() const {return hostImport && hostAlignment && GetMemoryHostPointerPropertiesEXT && GetPhysicalDeviceImageFormatProperties2;}
 void destroy(Sampler *s) {std::lock_guard<std::mutex> guard(mutex);if(s){if(!poisoned && s->sampler)DestroySampler(device,s->sampler,nullptr);delete s;}}
 Sampler *allocateSampler(bool minLinear,bool magLinear,uint32_t mip=0,float minLod=0,float maxLod=FLT_MAX) {
  std::lock_guard<std::mutex> guard(mutex);requireLive();auto *s=new Sampler{};s->lodMin=minLod;s->lodMax=maxLod;s->filters=1|(minLinear?2:0)|(magLinear?4:0)|(mip<<3);
  if(mip>2 || !std::isfinite(minLod) || !std::isfinite(maxLod) || minLod<0 || maxLod<minLod){delete s;throw std::runtime_error("sampler mip/LOD");}
  VkSamplerCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;ci.minFilter=minLinear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;
  ci.magFilter=magLinear?VK_FILTER_LINEAR:VK_FILTER_NEAREST;ci.mipmapMode=mip==2?VK_SAMPLER_MIPMAP_MODE_LINEAR:VK_SAMPLER_MIPMAP_MODE_NEAREST;
  ci.addressModeU=ci.addressModeV=ci.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;ci.minLod=mip?std::min(minLod,VK_LOD_CLAMP_NONE):0;ci.maxLod=mip?std::min(maxLod,VK_LOD_CLAMP_NONE):0;
  try {check(CreateSampler(device,&ci,nullptr,&s->sampler));return s;}catch(...) {delete s;throw;}
 }
 // The caller retains/locks the IOSurface; ABI 2 pins its task pages and handles
 // non-coherent cache handoff inside kernel submit/retirement.
 // No staging copy: only an external host allocation with exactly matching linear layout.
 Texture *importTexture(void *base,size_t bytes,size_t row,uint32_t width=256,uint32_t height=256) {
  std::unique_lock<std::mutex> guard(mutex);requireLive();
  if(!supportsHostImport())throw std::runtime_error("IOSurface same-memory import requires VK_EXT_external_memory_host / NativeAdreno HostImport");
  constexpr VkFormatFeatureFlags required=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
   VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if((linearFeatures&required)!=required)throw std::runtime_error("IOSurface linear BGRA8 format features unavailable");
  if(!base || !bytes || bytes>maxBufferLength || (uintptr_t(base)%hostAlignment) || bytes%hostAlignment || !limits.rows(width,height,4,row,0,bytes))
   throw std::runtime_error("IOSurface host alignment/length/stride");
  VkPhysicalDeviceExternalImageFormatInfo external{};external.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
  external.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
  VkPhysicalDeviceImageFormatInfo2 info{};info.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;info.pNext=&external;
  info.format=VK_FORMAT_B8G8R8A8_UNORM;info.type=VK_IMAGE_TYPE_2D;info.tiling=VK_IMAGE_TILING_LINEAR;
  info.usage=(orderedColorAccess?VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT:0)|VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  VkExternalImageFormatProperties ep{};ep.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 props{};props.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;props.pNext=&ep;
  check(GetPhysicalDeviceImageFormatProperties2(physical,&info,&props));
  auto features=ep.externalMemoryProperties.externalMemoryFeatures;
  if(!(features&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) || (features&VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) ||
     !(ep.externalMemoryProperties.compatibleHandleTypes&external.handleType) || props.imageFormatProperties.maxExtent.width<width || props.imageFormatProperties.maxExtent.height<height)
   throw std::runtime_error("IOSurface linear BGRA8 sampled/render/transfer import unsupported");
  auto *t=new Texture{};t->width=width;t->height=height;t->usage=info.usage;t->tiling=VK_IMAGE_TILING_LINEAR;t->imported=true;t->defined=true;t->layout=VK_IMAGE_LAYOUT_PREINITIALIZED;
  try {
   VkExternalMemoryImageCreateInfo ei{};ei.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;ei.handleTypes=external.handleType;
   VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.pNext=&ei;ii.imageType=info.type;ii.format=info.format;
   ii.extent={width,height,1};ii.mipLevels=1;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=info.tiling;ii.usage=info.usage;ii.initialLayout=t->layout;
   check(CreateImage(device,&ii,nullptr,&t->image));VkMemoryRequirements req{};GetImageMemoryRequirements(device,t->image,&req);
   VkImageSubresource sub{VK_IMAGE_ASPECT_COLOR_BIT,0,0};VkSubresourceLayout layout{};GetImageSubresourceLayout(device,t->image,&sub,&layout);
   if(layout.offset || layout.rowPitch!=row || req.size>bytes || layout.size>bytes)throw std::runtime_error("IOSurface/Vulkan linear image layout mismatch");
   VkMemoryHostPointerPropertiesEXT hp{};hp.sType=VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT;
   check(GetMemoryHostPointerPropertiesEXT(device,external.handleType,base,&hp));
   uint32_t type=0;while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&hp.memoryTypeBits&(1u<<type)) || !(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)))++type;
   if(type==memory.memoryTypeCount)throw std::runtime_error("IOSurface host memory type mismatch");
   VkImportMemoryHostPointerInfoEXT hi{};hi.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;hi.handleType=external.handleType;hi.pHostPointer=base;
   VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.pNext=&hi;ai.allocationSize=bytes;ai.memoryTypeIndex=type;t->memoryType=type;
   limits.reserve(ai.allocationSize);t->charged=ai.allocationSize;
   check(AllocateMemory(device,&ai,nullptr,&t->memory));++liveMemoryObjects;liveMemoryBytes+=ai.allocationSize;t->memoryBytes=ai.allocationSize;t->allocated=bytes;check(BindImageMemory(device,t->image,t->memory,0));
   check(MapMemory(device,t->memory,0,VK_WHOLE_SIZE,0,&t->mapped));
   VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=t->image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=info.format;
   vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};check(CreateImageView(device,&vi,nullptr,&t->view));return t;
  }catch(...) {guard.unlock();destroy(t);throw;}
 }
 static size_t texelBytes(uint32_t f){switch(f){case 1:case 10:return 1;case 30:return 2;case 115:return 8;case 70:case 80:case 81:case 94:return 4;default:throw std::runtime_error("unsupported pixel format");}}
 static VkFormat pixelFormat(uint32_t f) {
  switch(f){case 94:return VK_FORMAT_A2R10G10B10_UNORM_PACK32;case 80:return VK_FORMAT_B8G8R8A8_UNORM;case 81:return VK_FORMAT_B8G8R8A8_SRGB;case 70:return VK_FORMAT_R8G8B8A8_UNORM;case 115:return VK_FORMAT_R16G16B16A16_SFLOAT;case 10:case 1:return VK_FORMAT_R8_UNORM;case 30:return VK_FORMAT_R8G8_UNORM;default:throw std::runtime_error("unsupported pixel format");}
 }
 Texture *allocateTexture(uint32_t format=80,uint32_t usage=5,uint32_t width=256,uint32_t height=256,uint32_t levels=1) {
  std::unique_lock<std::mutex> guard(mutex);requireLive();
  if(!levels || levels>mipCount(width,height))throw std::runtime_error("texture mip count");
  if(!limits.extent(width,height,texelBytes(format)))throw std::runtime_error("texture extent/bytes outside device limit");
  if((usage&~21u) || format==81 || (format==1 && ((usage&4) || !usage)))throw std::runtime_error("unsupported texture usage/sRGB/A8 attachment");
  usage=(usage&5)?usage&5:(format==1?1:5);
  if(format==1 && (usage&4))throw std::runtime_error("A8 attachment");
  VkFormatProperties features{};GetPhysicalDeviceFormatProperties(physical,pixelFormat(format),&features);
  VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if(usage&1)needed|=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if(usage&4)needed|=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
  if((features.optimalTilingFeatures&needed)!=needed)throw std::runtime_error("texture format features unavailable");
  auto *t=new Texture{};t->width=width;t->height=height;t->levels=levels;t->format=format;t->vkFormat=pixelFormat(format);
  t->canGenerate=(features.optimalTilingFeatures&(VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))==(VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
  try {
   VkImageCreateInfo ii{};ii.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;ii.imageType=VK_IMAGE_TYPE_2D;ii.format=t->vkFormat;
   ii.extent={width,height,1};ii.mipLevels=levels;ii.arrayLayers=1;ii.samples=VK_SAMPLE_COUNT_1_BIT;ii.tiling=VK_IMAGE_TILING_OPTIMAL;
   ii.usage=(orderedColorAccess && (usage&4)?VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT:0)|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|(usage&4?VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:0)|(usage&1?VK_IMAGE_USAGE_SAMPLED_BIT:0);
   t->usage=ii.usage;check(CreateImage(device,&ii,nullptr,&t->image));VkMemoryRequirements req{};GetImageMemoryRequirements(device,t->image,&req);t->allocated=req.size;
   uint32_t type=0;while(type<memory.memoryTypeCount && (!(req.memoryTypeBits&(1u<<type)) || !(memory.memoryTypes[type].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))++type;
   if(type==memory.memoryTypeCount)throw std::runtime_error("no device-local texture memory");
   VkMemoryAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;ai.allocationSize=req.size;ai.memoryTypeIndex=type;t->memoryType=type;
   limits.reserve(ai.allocationSize);t->charged=ai.allocationSize;
   check(AllocateMemory(device,&ai,nullptr,&t->memory));++liveMemoryObjects;liveMemoryBytes+=ai.allocationSize;t->memoryBytes=ai.allocationSize;check(BindImageMemory(device,t->image,t->memory,0));
   VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=t->image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ii.format;
   vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,levels,0,1};check(CreateImageView(device,&vi,nullptr,&t->view));return t;
  }catch(...) {guard.unlock();destroy(t);throw;}
 }
 VkImageView attachmentView(Texture *t,uint32_t level) {
  if(t->levels==1)return t->view;
  if(t->attachmentViews.empty())t->attachmentViews.resize(t->levels);
  auto &view=t->attachmentViews[level];if(view)return view;
  VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=t->image;
  vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=t->vkFormat;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,level,1,0,1};
  check(CreateImageView(device,&vi,nullptr,&view));return view;
 }
 void destroyPipelineUnlocked(Pipeline *p) {
  if(!p || --p->references)return;for(auto &v:p->variants)destroyPipelineUnlocked(v.second);
  if(!poisoned) {
   if(p->pipeline)DestroyPipeline(device,p->pipeline,nullptr);
   if(p->stripPipeline)DestroyPipeline(device,p->stripPipeline,nullptr);
   if(p->layout)DestroyPipelineLayout(device,p->layout,nullptr);
   if(p->setLayout)DestroyDescriptorSetLayout(device,p->setLayout,nullptr);
   for(auto sampler:p->bakedSamplers)if(sampler)DestroySampler(device,sampler,nullptr);
   for(auto *b:p->constantBlobs)destroyUnlocked(b);
   for(auto entry:p->renders)if(auto render=entry.second){clearFrames(nullptr,render);DestroyRenderPass(device,render,nullptr);}
  }
  if(p->translation)Translator::get().free(p->translation);delete p;
 }
 void destroy(Pipeline *p){std::lock_guard<std::mutex> guard(mutex);destroyPipelineUnlocked(p);}

 bool supportsOrderedColorAccess() const {return orderedColorAccess;}
 bool requiresSampling(const Pipeline *p) const {return std::any_of(p->descriptors.begin(),p->descriptors.end(),[](const Descriptor &d){return d.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || d.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;});}
 Pipeline *compilePipeline(void *v,const char *vn,void *f,const char *fn,const CompileOptions &options=CompileOptions{}) {
  std::unique_lock<std::mutex> guard(mutex);requireLive();Translation translated{};auto &tr=Translator::get();char error[2048]{};
  auto start=std::chrono::steady_clock::now();
  auto enabledOptions=options;enabledOptions.orderedColorAccess=orderedColorAccess;enabledOptions.independentBlend=independentBlend;
  uint8_t identity[32]{};
  if(!tr.identity(v,vn,f,fn,&enabledOptions,identity,error,sizeof(error)))throw std::runtime_error(error);
  std::string inputKey((const char *)identity,32);
  if(auto *cached=objects.acquire(inputKey)){
   ++pipelineHits;fprintf(stderr,"FUXI MTL: pipeline cache HIT %.3f ms\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());return cached;
  }
  int ok=tr.compile(v,vn,f,fn,(void *)gipa,(uint64_t)instance,(uint64_t)physical,&enabledOptions,&translated,error,sizeof(error));
  double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
  if(ok)fprintf(stderr,"FUXI MTL: translation PASS %.3f ms (bounded translator cache)\n",ms);
  else fprintf(stderr,"FUXI MTL: translation FAIL %.160s\n",error);
  if(!ok)throw std::runtime_error(error[0]?error:"translation failed");
  uint8_t hash[32]{};
  if(!tr.product_hash(translated.handle,hash)){tr.free(translated.handle);throw std::runtime_error("pipeline product hash");}
  std::string productKey=inputKey+std::string((const char *)hash,32);
  if(auto *cached=objects.acquire(productKey,true)){
   tr.free(translated.handle);++pipelineHits;
   fprintf(stderr,"FUXI MTL: pipeline cache HIT %.3f ms\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());return cached;
  }
  ++pipelineMisses;
  auto *p=createPipeline(translated,enabledOptions);
  try {objects.insert(inputKey,productKey,p,p->retainedBytes,[&](Pipeline *old){destroyPipelineUnlocked(old);});}
  catch(...){destroyPipelineUnlocked(p);throw;}
  fprintf(stderr,"FUXI MTL: pipeline cache MISS %.3f ms\n",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());
  return p;
 }
 VkRenderPass renderPass(Pipeline *p,const Operation *op) {
  unsigned count=p->options.colorCount;uint32_t key=0;
  for(unsigned i=0;i<count;++i){auto c=op?op->color(i):ColorTarget{};key|=(c.load*2+c.store)<<(i*3);}
  for(auto e:p->renders)if(e.first==key)return e.second;
  if(p->renders.size()>=256)throw std::runtime_error("render pass action variant bound 256");
  VkAttachmentDescription attachments[8]{};VkAttachmentReference refs[8]{};unsigned used=0;
  for(unsigned i=0;i<count;++i){if(!p->options.color(i).format){refs[i]={VK_ATTACHMENT_UNUSED,VK_IMAGE_LAYOUT_GENERAL};continue;}auto &a=attachments[used];auto c=op?op->color(i):ColorTarget{};
   a.format=pixelFormat(p->options.color(i).format);a.samples=VK_SAMPLE_COUNT_1_BIT;
   a.loadOp=c.load==2?VK_ATTACHMENT_LOAD_OP_CLEAR:c.load==1?VK_ATTACHMENT_LOAD_OP_LOAD:VK_ATTACHMENT_LOAD_OP_DONT_CARE;
   a.storeOp=c.store?VK_ATTACHMENT_STORE_OP_STORE:VK_ATTACHMENT_STORE_OP_DONT_CARE;
   a.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
   a.initialLayout=a.finalLayout=VK_IMAGE_LAYOUT_GENERAL;
   refs[i]={used++,orderedColorAccess?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  }
  VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=count;sub.pColorAttachments=refs;
  if(orderedColorAccess){sub.flags=VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT;sub.inputAttachmentCount=count;sub.pInputAttachments=refs;}
  VkSubpassDependency deps[2]{};deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;
  deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;deps[1].dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
  if(orderedColorAccess){deps[0].dstStageMask|=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;deps[0].dstAccessMask|=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;deps[1].srcStageMask|=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;deps[1].srcAccessMask|=VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;}
  VkRenderPassCreateInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;ri.attachmentCount=used;ri.pAttachments=attachments;ri.subpassCount=1;ri.pSubpasses=&sub;ri.dependencyCount=2;ri.pDependencies=deps;
  VkRenderPass render{};check(CreateRenderPass(device,&ri,nullptr,&render));
  try{p->renders.push_back({key,render});}catch(...){DestroyRenderPass(device,render,nullptr);throw;}return render;
 }
 Pipeline *createPipeline(Translation translated,const CompileOptions &options) {
  auto &tr=Translator::get();auto *p=new Pipeline{};p->options=options;VkShaderModule modules[2]{};
  auto cleanup=[&]{for(auto m:modules)if(m)DestroyShaderModule(device,m,nullptr);if(translated.handle)tr.free(translated.handle);};
  try {
   p->retainedBytes=sizeof(Pipeline)+(translated.vertexCount+translated.fragmentCount)*4;
   for(unsigned i=0;i<translated.blobCount && i<2;++i)p->retainedBytes+=translated.blobs[i].length;
   if(!validFetchContract(orderedColorAccess,translated.orderedColorAccess,translated.fetchMask))throw std::runtime_error("ordered framebuffer fetch plan/device mismatch");
   uint32_t presentMask=0;for(unsigned i=0;i<options.colorCount && i<8;++i)if(options.color(i).format)presentMask|=1u<<i;
   if(translated.fetchMask&~presentMask)throw std::runtime_error("fetch references absent color attachment");
   p->fetchMask=translated.fetchMask;p->orderedColorAccess=orderedColorAccess;
   if(!options.colorCount || options.colorCount>maxColorAttachments)throw std::runtime_error("color attachment count exceeds device");
   for(unsigned i=0;i<options.colorCount;++i){auto c=options.color(i);if(!c.format)continue;VkFormatProperties features{};GetPhysicalDeviceFormatProperties(physical,pixelFormat(c.format),&features);
    auto needed=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|(translated.colors[i].blend?VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT:0);
    if((features.optimalTilingFeatures&needed)!=(uint32_t)needed)throw std::runtime_error("attachment["+std::to_string(i)+"] format/blend unavailable");
   }
   p->render=renderPass(p,nullptr);
   p->pushOffset=translated.pushOffset;p->pushSize=translated.pushSize;p->pushStages=translated.pushStages;memcpy(p->pushBytes,translated.pushBytes,128);
   // ABI 3 exposes both stage ranges. The translator limits each to the 128-byte Vulkan minimum.
   VkPushConstantRange ranges[2]{};if(translated.rangeCount>2)throw std::runtime_error("push ranges");
   for(unsigned i=0;i<translated.rangeCount;++i){auto r=translated.ranges[i];ranges[i]={r.stages,r.offset,r.size};}
   if(translated.blobCount>2)throw std::runtime_error("constant blob count");
   for(unsigned i=0;i<translated.blobCount;++i){const auto &blob=translated.blobs[i];
    if(!blob.bytes || !blob.length || blob.length>4*1024*1024)throw std::runtime_error("constant blob bounds");
    auto *buffer=allocateUnlocked((blob.length+15)&~size_t(15));p->constantBlobs.push_back(buffer);
    memcpy(buffer->contents,blob.bytes,blob.length);
    std::array<uint8_t,32> hash{};std::copy(blob.hash,blob.hash+32,hash.begin());p->blobHashes.push_back(hash);
   }
   if(translated.descriptorCount>16)throw std::runtime_error("descriptor count>16");
   std::vector<VkDescriptorSetLayoutBinding> bindings;
   for(uint32_t i=0;i<translated.descriptorCount;++i) {
    auto d=translated.descriptors[i];
    if((d.type!=VK_DESCRIPTOR_TYPE_SAMPLER && d.type!=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE && d.type!=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && d.type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER && d.type!=VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT) || !d.stages || (d.stages&~17u))
     throw std::runtime_error("unsupported translator descriptor");
    if(d.type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT && (!translated.fetchMask || d.stages!=VK_SHADER_STAGE_FRAGMENT_BIT))throw std::runtime_error("input attachment layout without fetch plan");
    if(d.set || d.source>1)throw std::runtime_error("descriptor set/source");
    if(d.source==1 && ((d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && d.type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) || d.slot>=p->constantBlobs.size() || d.offset || d.range!=p->constantBlobs[d.slot]->length))throw std::runtime_error("constant blob layout mismatch");
    if(d.binding>=16)throw std::runtime_error("descriptor binding limit");
    BakedSampler baked{};char why[256]{};
    if(!tr.sampler(translated.handle,d.binding,&baked,why,sizeof(why)))throw std::runtime_error(why);
    if(baked.enabled){VkSamplerCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
     si.magFilter=(VkFilter)baked.mag;si.minFilter=(VkFilter)baked.min;si.mipmapMode=(VkSamplerMipmapMode)baked.mip;
     si.addressModeU=(VkSamplerAddressMode)baked.address[0];si.addressModeV=(VkSamplerAddressMode)baked.address[1];si.addressModeW=(VkSamplerAddressMode)baked.address[2];
     si.mipLodBias=baked.bias;si.minLod=baked.minLod;si.maxLod=baked.maxLod;si.borderColor=(VkBorderColor)baked.border;
     check(CreateSampler(device,&si,nullptr,&p->bakedSamplers[d.binding]));
    }
    bindings.push_back({d.binding,(VkDescriptorType)d.type,1,d.stages,baked.enabled?&p->bakedSamplers[d.binding]:nullptr});p->descriptors.push_back(d);
   }
   if(!bindings.empty()) {
    VkDescriptorSetLayoutCreateInfo si{};si.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;si.bindingCount=bindings.size();si.pBindings=bindings.data();
    check(CreateDescriptorSetLayout(device,&si,nullptr,&p->setLayout));
   }
   VkPipelineLayoutCreateInfo li{};li.setLayoutCount=p->setLayout?1:0;li.pSetLayouts=&p->setLayout;li.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;li.pushConstantRangeCount=translated.rangeCount;li.pPushConstantRanges=ranges;check(CreatePipelineLayout(device,&li,nullptr,&p->layout));
   const uint32_t *words[]={translated.vertex,translated.fragment};size_t counts[]={translated.vertexCount,translated.fragmentCount};
   for(unsigned i=0;i<2;++i) {
    if(!words[i] || counts[i]<5 || counts[i]>1024*1024 || words[i][0]!=0x07230203)throw std::runtime_error("invalid translator SPIR-V");
    VkShaderModuleCreateInfo mi{};mi.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;mi.codeSize=counts[i]*4;mi.pCode=words[i];check(CreateShaderModule(device,&mi,nullptr,&modules[i]));
   }
   if(translated.specCount>32 || translated.specSize>256)throw std::runtime_error("specialization bounds");
   std::vector<VkSpecializationMapEntry> specEntries;
   for(uint32_t i=0;i<translated.specCount;++i) {
    auto e=translated.specs[i];if(e.offset>translated.specSize || e.size>translated.specSize-e.offset || !(e.size==1 || e.size==2 || e.size==4 || e.size==8))throw std::runtime_error("specialization entry");
    specEntries.push_back({e.id,e.offset,e.size});
   }
   VkSpecializationInfo specialization{(uint32_t)specEntries.size(),specEntries.data(),translated.specSize,translated.specData};
   VkPipelineShaderStageCreateInfo stages[2]{};
   for(unsigned i=0;i<2;++i) {stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[i].module=modules[i];stages[i].pSpecializationInfo=&specialization;}
   stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].pName=translated.vertexEntry;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].pName=translated.fragmentEntry;
   VkPipelineVertexInputStateCreateInfo vertex{};vertex.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
   std::vector<VkVertexInputBindingDescription> vbindings;std::vector<VkVertexInputAttributeDescription> vattrs;
   if(translated.bindingCount>4 || translated.attributeCount>8)throw std::runtime_error("vertex input bounds");
   for(unsigned i=0;i<translated.bindingCount;++i){auto b=translated.bindings[i];vbindings.push_back({b.binding,b.stride,VK_VERTEX_INPUT_RATE_VERTEX});}
   for(unsigned i=0;i<translated.attributeCount;++i){auto a=translated.attributes[i];
    VkFormatProperties support{};GetPhysicalDeviceFormatProperties(physical,(VkFormat)a.format,&support);
    if(!(support.bufferFeatures&VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))throw std::runtime_error("vertex attribute["+std::to_string(a.location)+"] Vulkan format="+std::to_string(a.format)+" lacks VERTEX_BUFFER");
    vattrs.push_back({a.location,a.binding,(VkFormat)a.format,a.offset});}
   vertex.vertexBindingDescriptionCount=vbindings.size();vertex.pVertexBindingDescriptions=vbindings.data();vertex.vertexAttributeDescriptionCount=vattrs.size();vertex.pVertexAttributeDescriptions=vattrs.data();
   VkPipelineInputAssemblyStateCreateInfo assembly{};assembly.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
   // Metal's viewport has a top-left origin. Vulkan 1.2 supports negative height.
   VkViewport viewport{0,256,256,-256,0,1};VkRect2D scissor{{0,0},{256,256}};
   VkPipelineViewportStateCreateInfo vp{};vp.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
   VkPipelineRasterizationStateCreateInfo raster{};raster.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
   VkPipelineMultisampleStateCreateInfo ms{};ms.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
   VkPipelineColorBlendAttachmentState blends[8]{};
   for(unsigned i=0;i<options.colorCount;++i){auto &blend=blends[i];auto c=translated.colors[i];blend.colorWriteMask=c.mask;blend.blendEnable=c.blend;
    blend.srcColorBlendFactor=(VkBlendFactor)c.factors[0];blend.dstColorBlendFactor=(VkBlendFactor)c.factors[1];blend.colorBlendOp=(VkBlendOp)c.factors[2];blend.srcAlphaBlendFactor=(VkBlendFactor)c.factors[3];blend.dstAlphaBlendFactor=(VkBlendFactor)c.factors[4];blend.alphaBlendOp=(VkBlendOp)c.factors[5];}
   VkPipelineColorBlendStateCreateInfo bs{};bs.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;bs.attachmentCount=options.colorCount;bs.pAttachments=blends;if(orderedColorAccess)bs.flags=VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;

   VkGraphicsPipelineCreateInfo gi{};gi.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;gi.stageCount=2;gi.pStages=stages;gi.pVertexInputState=&vertex;gi.pInputAssemblyState=&assembly;
   gi.pViewportState=&vp;gi.pRasterizationState=&raster;gi.pMultisampleState=&ms;gi.pColorBlendState=&bs;gi.layout=p->layout;gi.renderPass=p->render;
   VkDynamicState dynamicStates[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};VkPipelineDynamicStateCreateInfo dynamic{};dynamic.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;dynamic.dynamicStateCount=2;dynamic.pDynamicStates=dynamicStates;gi.pDynamicState=&dynamic;
   auto cache=pipelineCache.get();
   auto began=std::chrono::steady_clock::now();
   try {
    check(CreateGraphicsPipelines(device,cache,1,&gi,nullptr,&p->pipeline));assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;assembly.primitiveRestartEnable=VK_TRUE;
    check(CreateGraphicsPipelines(device,cache,1,&gi,nullptr,&p->stripPipeline));
   }catch(...) {pipelineCache.finishedPair(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count());throw;}
   auto buildMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-began).count();
   pipelineCache.finishedPair(buildMs);
   fprintf(stderr,"FUXI MTL: Vulkan pipelines PASS %.3f ms cache=%u\n",buildMs,cache!=VK_NULL_HANDLE);
   fprintf(stderr,"FUXI MTL: framebuffer fetch plan mask=%u ordered=%u\n",p->fetchMask,p->orderedColorAccess);
   p->translation=translated.handle;translated.handle=nullptr;cleanup();return p;
  }catch(...) {cleanup();destroyPipelineUnlocked(p);throw;}
 }
 void submit(std::vector<Operation> ops,const std::function<void()> &scheduled={}) {
  auto submitStart=std::chrono::steady_clock::now();auto submitQueued=submitStart,submitDone=submitStart;
  auto since=[](std::chrono::steady_clock::time_point t){return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-t).count();};
  std::lock_guard<std::mutex> guard(mutex);requireLive();
  // Empty completion participates in the same lock/order, but creates no BO or GPU fence.
  if(ops.empty()){++emptySubmits;if(scheduled)scheduled();return;}
  bool submitted=false,completed=false;std::vector<VkImageView> sampleViews;std::vector<VkFramebuffer> frames;std::vector<VkRenderPass> emptyRenders;std::vector<Texture *> textures;
  struct TextureState {Texture *texture;VkImageLayout layout;bool defined;uint32_t definedLevels;};std::vector<TextureState> oldTextures;
  size_t diagnosticOp=0;Buffer *neutralBuffer=nullptr;
  auto cleanup=[&] {destroyUnlocked(neutralBuffer);for(auto v:sampleViews)DestroyImageView(device,v,nullptr);for(auto frame:frames)DestroyFramebuffer(device,frame,nullptr);for(auto render:emptyRenders)DestroyRenderPass(device,render,nullptr);};
  // Exclusive prepare phases: object API/cache time is subtracted from recording.
  uint64_t objectNanos=0,descriptorNanos=0;
  struct Measure {uint64_t &total;std::chrono::steady_clock::time_point start=std::chrono::steady_clock::now();~Measure(){total+=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();}};
  try {
   std::vector<DrawOutput> draws(ops.size());
   std::vector<Buffer *> buffers;
   auto addBuffer=[&](Buffer *b){if(b && std::find(buffers.begin(),buffers.end(),b)==buffers.end())buffers.push_back(b);};
   auto addTexture=[&](Texture *t){if(t && std::find(textures.begin(),textures.end(),t)==textures.end())textures.push_back(t);};
   for(size_t n=0;n<ops.size();++n){diagnosticOp=n;auto &op=ops[n];addBuffer(op.src);addBuffer(op.dst);addBuffer(op.indices);addTexture(op.texture);addTexture(op.sourceTexture);
    if(op.texture && op.dstLevel>=op.texture->levels)throw std::runtime_error("destination mip level");
    if(op.texture && op.srcLevel>=(op.sourceTexture?op.sourceTexture:op.texture)->levels)throw std::runtime_error("source mip level");
    if(op.generateMips){if(!op.texture || !op.mipCount || op.mipBase>=op.texture->levels || op.mipCount>op.texture->levels-op.mipBase || !op.texture->canGenerate)throw std::runtime_error("mipmap generation range/format");continue;}
    if(!op.pipeline && op.texture && !op.clearOnly){
     auto *src=op.sourceTexture?op.sourceTexture:op.texture;auto *dst=op.texture;
     uint32_t sw=src->levelWidth(op.srcLevel),sh=src->levelHeight(op.srcLevel),dw=dst->levelWidth(op.dstLevel),dh=dst->levelHeight(op.dstLevel);
     size_t width=op.width?op.width:(op.upload||op.sourceTexture?dw:sw),height=op.height?op.height:(op.upload||op.sourceTexture?dh:sh),pixel=texelBytes(dst->format);
     size_t row=op.rowBytes?op.rowBytes:width*pixel;
     if(!width || !height || row<width*pixel || row%pixel || row>maxBufferLength/height ||
        ((op.upload || op.sourceTexture) && (op.dstX>dw || width>dw-op.dstX || op.dstY>dh || height>dh-op.dstY)) ||
        (!op.upload && (op.srcX>sw || width>sw-op.srcX || op.srcY>sh || height>sh-op.srcY)))throw std::runtime_error("texture transfer region/stride bounds");
     size_t bytes=(height-1)*row+width*pixel;
     if(op.sourceTexture && (src->format!=dst->format || (src==dst?op.srcLevel==op.dstLevel:src->memory==dst->memory || hostRangesOverlap(src->mapped,src->allocated,dst->mapped,dst->allocated))))throw std::runtime_error("texture copy format/alias");
     auto *transferBuffer=op.upload?op.src:op.sourceTexture?nullptr:op.dst;
     if(transferBuffer && (transferBuffer->memory==dst->memory || hostRangesOverlap(transferBuffer->contents,transferBuffer->length,dst->mapped,dst->allocated)))throw std::runtime_error("image/buffer transfer alias");
     if(op.upload && (!op.src || op.from%4 || op.from%pixel || op.from>op.src->length || bytes>op.src->length-op.from))throw std::runtime_error("upload bounds/alignment");
     if(!op.upload && !op.sourceTexture && (!op.dst || op.to%4 || op.to%pixel || op.to>op.dst->length || bytes>op.dst->length-op.to))throw std::runtime_error("readback bounds/alignment");
    }
    if(op.pipeline || op.clearOnly){
     if(!op.colorCount || op.colorCount>maxColorAttachments || !op.colorMask || (op.colorMask&~((1u<<op.colorCount)-1)) || (op.pipeline && op.colorCount!=op.pipeline->options.colorCount))throw std::runtime_error("draw/pass color count mismatch");
     for(unsigned i=0;i<op.colorCount;++i){auto c=op.color(i);auto *t=c.texture;
      if(!t){if(op.pipeline && op.pipeline->options.color(i).format)throw std::runtime_error("missing render attachment");continue;}addTexture(t);
      if(!t || c.level>=t->levels || c.load>2 || c.store>1 || (op.pipeline && t->format!=op.pipeline->options.color(i).format))throw std::runtime_error("attachment["+std::to_string(i)+"] target/level/format/actions");
      if(op.areaWidth>t->levelWidth(c.level) || op.areaHeight>t->levelHeight(c.level))throw std::runtime_error("render area exceeds attachment");
      if(t->levelWidth(c.level)!=op.texture->levelWidth(op.dstLevel) || t->levelHeight(c.level)!=op.texture->levelHeight(op.dstLevel))throw std::runtime_error("attachment size mismatch");
      if(orderedColorAccess && op.pipeline && !(t->usage&VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT))throw std::runtime_error("ordered target lacks INPUT_ATTACHMENT");
      if(op.pipeline && (op.pipeline->fetchMask&(1u<<i)) && c.load==0)throw std::runtime_error("fetch from DontCare attachment");
      if(op.pipeline && t->tiling==VK_IMAGE_TILING_LINEAR && op.pipeline->options.color(i).blend){VkFormatProperties props{};GetPhysicalDeviceFormatProperties(physical,t->vkFormat,&props);if(!(props.linearTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT))throw std::runtime_error("linear attachment blending unavailable");}
      for(unsigned j=0;j<i;++j){auto other=op.color(j);if(!other.texture)continue;if(t->memory==other.texture->memory || hostRangesOverlap(t->mapped,t->allocated,other.texture->mapped,other.texture->allocated))throw std::runtime_error("render attachment alias");}
      for(unsigned stage=0;stage<2;++stage)for(auto binding:op.buffers[stage])if(binding.buffer && (binding.buffer->memory==t->memory || hostRangesOverlap(binding.buffer->contents,binding.buffer->length,t->mapped,t->allocated)))throw std::runtime_error("attachment/buffer feedback");
      for(unsigned slot=0;slot<resourceSlots;++slot){auto *sample=op.samples[slot];if(!slot && !sample)sample=op.sourceTexture;
       if(sample && (sample->memory==t->memory || hostRangesOverlap(sample->mapped,sample->allocated,t->mapped,t->allocated)))throw std::runtime_error("attachment feedback");}
     }
    }
    if(op.clearOnly && (!op.texture || op.pipeline || op.load>2 || op.store>1))throw std::runtime_error("invalid empty render pass");
    if(!op.pipeline)continue;
    if(op.vertices<3 || op.vertices>65536 || op.first>65536-op.vertices || (!op.strip && op.vertices%3))throw std::runtime_error("invalid primitive count/start");
    if(!op.texture || op.load>2 || op.store>1)throw std::runtime_error("invalid bounded draw target/actions");
    const auto &v=op.viewport;const auto &r=op.scissor;
    if(!viewportFits(v.x,v.y+v.height,v.width,-v.height,v.minDepth,v.maxDepth,op.texture->levelWidth(op.dstLevel),op.texture->levelHeight(op.dstLevel)) || r.offset.x<0 || r.offset.y<0 ||
       op.areaWidth>op.texture->levelWidth(op.dstLevel) || op.areaHeight>op.texture->levelHeight(op.dstLevel) ||
       !scissorFits(r.offset.x,r.offset.y,r.extent.width,r.extent.height,op.areaWidth?op.areaWidth:op.texture->levelWidth(op.dstLevel),op.areaHeight?op.areaHeight:op.texture->levelHeight(op.dstLevel)))throw std::runtime_error("viewport/scissor target bounds");
    DrawInput input{};input.count=op.vertices;input.start=op.first;input.strip=op.strip;input.indexed=op.indices!=nullptr;input.indexLength=op.indices?op.indices->length:0;input.indexOffset=op.indexOffset;
    for(unsigned stage=0;stage<2;++stage)for(unsigned i=0;i<resourceSlots;++i){auto b=op.buffers[stage][i];if(b.buffer){if(b.buffer->memory==op.texture->memory || hostRangesOverlap(b.buffer->contents,b.buffer->length,op.texture->mapped,op.texture->allocated))throw std::runtime_error("attachment/buffer feedback");addBuffer(b.buffer);input.buffers[stage][i]={b.buffer->length,b.offset,b.bytes?1u:0u};}}
    for(unsigned i=0;i<resourceSlots;++i){auto *t=i==0 && !op.samples[i]?op.sourceTexture:op.samples[i];auto *sam=i==0 && !op.samplers[i]?op.sampler:op.samplers[i];
     if(t){uint32_t base=op.sampleBase[i],count=op.sampleCount[i]?op.sampleCount[i]:t->levels-base;
      if(base>=t->levels || !count || count>t->levels-base)throw std::runtime_error("sample mip range");
      if(t==op.texture || (t->memory==op.texture->memory || hostRangesOverlap(t->mapped,t->allocated,op.texture->mapped,op.texture->allocated)))throw std::runtime_error("attachment feedback");addTexture(t);input.formats[i]=t->format;}
     if(sam){input.filters[i]=sam->filters;input.lodMin[i]=sam->lodMin;input.lodMax[i]=sam->lodMax;}
    }
    char why[512]{};auto &translator=Translator::get();
    if(!translator.draw(op.pipeline->translation,&input,&draws[n],why,sizeof(why))){
     if(strcmp(why,"draw requires runtime variant") && strcmp(why,"draw requires different specialization"))throw std::runtime_error(why);
     std::string key;for(unsigned i=0;i<resourceSlots;++i){key+=std::to_string(input.formats[i])+","+std::to_string(input.filters[i])+",";key.append((const char *)&input.lodMin[i],sizeof(float));key.append((const char *)&input.lodMax[i],sizeof(float));for(auto &stage:input.buffers)key+=std::to_string(stage[i].length!=0)+":"+std::to_string(stage[i].bytes)+",";}
     auto *original=op.pipeline;auto found=std::find_if(original->variants.begin(),original->variants.end(),[&](const auto &v){return v.first==key;});
     if(found!=original->variants.end())op.pipeline=found->second;
     else {if(original->variants.size()>=8)throw std::runtime_error("runtime variant bound");Translation variant{};
      if(!translator.variant(original->translation,&input,&variant,why,sizeof(why)))throw std::runtime_error(why);
      auto *pipeline=createPipeline(variant,original->options);try{original->variants.push_back({key,pipeline});}catch(...){destroyPipelineUnlocked(pipeline);throw;}original->retainedBytes+=pipeline->retainedBytes;objects.reweigh(original,original->retainedBytes,[&](Pipeline *old){destroyPipelineUnlocked(old);});op.pipeline=pipeline;
     }
     if(!translator.draw(op.pipeline->translation,&input,&draws[n],why,sizeof(why)))throw std::runtime_error(why);
    }
    for(auto *blob:op.pipeline->constantBlobs)addBuffer(blob);
    // CPU bounds check the fixed-function vertex fetches, including every index.
    uint32_t last=op.first+op.vertices-1;
    if(op.indices){last=0;for(unsigned i=0;i<op.vertices;++i){uint16_t index;memcpy(&index,(uint8_t *)op.indices->contents+op.indexOffset+2*i,2);if(op.strip && index==65535)continue;last=std::max(last,uint32_t(index));}}
    for(auto &a:op.pipeline->options.attributes)if(a.format){auto slot=a.buffer;
     if(slot>=4 || !op.buffers[0][slot].buffer)throw std::runtime_error("unbound vertex buffer");
     auto bound=op.buffers[0][slot];
     if(!vertexFetchFits(bound.buffer->length,bound.offset,last,op.pipeline->options.strides[slot],a.offset,a.format))throw std::runtime_error("vertex fetch bounds");
    }
    // CPU index bounds are valid only for quiescent host input. A preceding GPU
    // write in this command needs readback before its values can be checked.
    for(size_t previous=0;previous<n;++previous)if(ops[previous].dst){
     if(ops[previous].dst==op.indices)throw std::runtime_error("GPU-written index input requires separate submission");
     for(auto &stage:op.buffers)for(auto &bound:stage)if(bound.buffer==ops[previous].dst)throw std::runtime_error("GPU-written draw buffer requires separate submission");
    }
   }
   size_t neutralSize=0;for(const auto &draw:draws)for(unsigned i=0;i<draw.descriptorCount;++i){const auto &d=draw.descriptors[i];if(d.source==2){
    if((d.type!=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && d.type!=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) || d.offset || !d.range || d.range>maxBufferLength)throw std::runtime_error("planned neutral buffer bounds/type");
    neutralSize=std::max(neutralSize,size_t(d.range));}}
   if(neutralSize){Measure measure{objectNanos};neutralBuffer=allocateUnlocked(neutralSize);addBuffer(neutralBuffer);}
   for(auto *t:textures)oldTextures.push_back({t,t->layout,t->defined,t->definedLevels});
   auto flushStart=std::chrono::steady_clock::now();
   // Direct shared host writes are not tracked. Only Vulkan HOST_COHERENT permits omission.
   for(auto *t:textures)if(t->mapped && !coherent(t->memoryType)) {VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=t->memory;r.size=VK_WHOLE_SIZE;++flushCalls;check(FlushMappedMemoryRanges(device,1,&r));}
   for(auto *b:buffers)if(!coherent(b->memoryType)) {VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=b->memory;r.size=VK_WHOLE_SIZE;++flushCalls;check(FlushMappedMemoryRanges(device,1,&r));}
   submitFlushNanos+=since(flushStart);
   {Measure measure{objectNanos};
    if(!commandPool){VkCommandPoolCreateInfo pi{};pi.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pi.queueFamilyIndex=family;check(CreateCommandPool(device,&pi,nullptr,&commandPool));++reuseCounts.commandPools;++submitObjectCreates;++submitPoolCreates;
     VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;ai.commandPool=commandPool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
     check(AllocateCommandBuffers(device,&ai,&command));++reuseCounts.commandBuffers;++submitObjectCreates;++submitPoolCreates;
    }else check(ResetCommandPool(device,commandPool,0));
    if(!submitFence){VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;check(CreateFence(device,&fi,nullptr,&submitFence));++reuseCounts.fences;++submitObjectCreates;++submitPoolCreates;}
    else check(ResetFences(device,1,&submitFence));
    if(supportsTimestamps() && !queryPool){VkQueryPoolCreateInfo qi{};qi.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=2;check(CreateQueryPool(device,&qi,nullptr,&queryPool));++reuseCounts.queries;++submitObjectCreates;++submitPoolCreates;}
   }
   VkCommandBuffer cmd=command;VkQueryPool timestamps=queryPool;VkFence fence=submitFence;
   auto recordStart=std::chrono::steady_clock::now();uint64_t recordObjects=objectNanos;
   VkCommandBufferBeginInfo bi{};bi.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(BeginCommandBuffer(cmd,&bi));
   if(timestamps){CmdResetQueryPool(cmd,timestamps,0,2);CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,timestamps,0);}
   VkMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_MEMORY_BARRIER;barrier.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
   CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
   auto transition=[&](Texture *t,VkImageLayout next) {
    VkImageMemoryBarrier ib{};ib.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;ib.oldLayout=t->layout;ib.newLayout=next;
    ib.srcAccessMask=t->layout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT;
    ib.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    ib.image=t->image;ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,t->levels,0,1};
    CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&ib);t->layout=next;
   };
   // Bound each descriptor type and sets independently; grow geometrically, retain one pool.
   VkDescriptorPool submitPool{};
   {Measure measure{objectNanos};std::vector<VkDescriptorPoolSize> sizes;uint32_t sets=0;
    for(size_t i=0;i<ops.size();++i){if(!ops[i].texture || !ops[i].pipeline || !draws[i].descriptorCount)continue;if(sets==4096)throw std::runtime_error("submit descriptor set bound");++sets;
     for(unsigned j=0;j<draws[i].descriptorCount;++j){auto ty=(VkDescriptorType)draws[i].descriptors[j].type;auto it=std::find_if(sizes.begin(),sizes.end(),[&](const VkDescriptorPoolSize &z){return z.type==ty;});if(it==sizes.end())sizes.push_back({ty,1});else {if(it->descriptorCount==65536)throw std::runtime_error("submit descriptor type bound");++it->descriptorCount;}}}
    if(sets>4096)throw std::runtime_error("submit descriptor set bound");
    bool grow=sets>descriptorSets;
    for(auto z:sizes){if(z.descriptorCount>65536)throw std::runtime_error("submit descriptor type bound");auto it=std::find_if(descriptorCapacity.begin(),descriptorCapacity.end(),[&](auto c){return c.type==z.type;});if(it==descriptorCapacity.end() || it->descriptorCount<z.descriptorCount)grow=true;}
    if(grow){
     auto capacity=descriptorCapacity;
     auto round=[](uint32_t n){uint32_t c=1;while(c<n)c*=2;return c;};
     for(auto z:sizes){auto it=std::find_if(capacity.begin(),capacity.end(),[&](auto c){return c.type==z.type;});if(it==capacity.end())capacity.push_back({z.type,round(z.descriptorCount)});else it->descriptorCount=std::max(it->descriptorCount,round(z.descriptorCount));}
     // Destroy the quiescent old pool first: avoid temporarily doubling kernel BOs.
     if(descriptorPool)DestroyDescriptorPool(device,descriptorPool,nullptr);descriptorPool={};
     VkDescriptorPoolCreateInfo pci{};pci.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;pci.maxSets=std::max(descriptorSets,round(sets));pci.poolSizeCount=capacity.size();pci.pPoolSizes=capacity.data();
     check(CreateDescriptorPool(device,&pci,nullptr,&descriptorPool));++reuseCounts.descriptors;++submitObjectCreates;++submitPoolCreates;descriptorSets=pci.maxSets;descriptorCapacity=std::move(capacity);
    }else if(descriptorPool)check(ResetDescriptorPool(device,descriptorPool,0));
    submitPool=descriptorPool;
   }
   for(size_t opIndex=0;opIndex<ops.size();++opIndex) {diagnosticOp=opIndex;const auto &op=ops[opIndex];const auto &draw=draws[opIndex];
    for(unsigned slot=0;slot<resourceSlots;++slot)if(auto *t=op.samples[slot]){
     uint32_t base=op.sampleBase[slot],count=op.sampleCount[slot]?op.sampleCount[slot]:t->levels-base;
     if(base>=t->levels || !count || count>t->levels-base)throw std::runtime_error("sample mip range");
     for(uint32_t l=base;l<base+count;++l)if(!t->isDefined(l))throw std::runtime_error("undefined sampled mip");
     transition(t,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if(op.sourceTexture && !op.sourceTexture->isDefined(op.srcLevel))throw std::runtime_error("undefined source texture mip");
    if(op.sourceTexture)transition(op.sourceTexture,op.pipeline?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_GENERAL);
    if(op.texture)transition(op.texture,VK_IMAGE_LAYOUT_GENERAL);
    if(op.pipeline || op.clearOnly)for(unsigned i=0;i<op.colorCount;++i)if(auto *target=op.color(i).texture)if(target!=op.texture)transition(target,VK_IMAGE_LAYOUT_GENERAL);
    if(op.generateMips){auto *t=op.texture;
     if(!t->isDefined(op.mipBase))throw std::runtime_error("undefined mip generation source");
     for(uint32_t l=op.mipBase+1;l<op.mipBase+op.mipCount;++l){
      VkImageBlit region{};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l-1,0,1};region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,l,0,1};
      region.srcOffsets[1]={(int32_t)t->levelWidth(l-1),(int32_t)t->levelHeight(l-1),1};region.dstOffsets[1]={(int32_t)t->levelWidth(l),(int32_t)t->levelHeight(l),1};
      CmdBlitImage(cmd,t->image,VK_IMAGE_LAYOUT_GENERAL,t->image,VK_IMAGE_LAYOUT_GENERAL,1,&region,VK_FILTER_LINEAR);
      barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
      CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);t->setDefined(l);
     }
    }else
    if(op.texture && op.pipeline) {
     std::array<Texture *,8> targets{};std::array<uint32_t,8> levels{};VkImageView targetViews[8]{},compactViews[8]{};VkClearValue clears[8]{};unsigned used=0;
     for(unsigned i=0;i<op.colorCount;++i){auto c=op.color(i);if(!c.texture)continue;if(c.load==1 && !c.texture->isDefined(c.level))throw std::runtime_error("load undefined attachment["+std::to_string(i)+"]");
      c.texture->setDefined(c.level,c.store!=0);targets[i]=c.texture;levels[i]=c.level;targetViews[i]=attachmentView(c.texture,c.level);compactViews[used]=targetViews[i];memcpy(clears[used++].color.float32,c.clear,sizeof(c.clear));}
     auto render=renderPass(op.pipeline,&op);
     VkFramebufferCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fi.renderPass=render;
     fi.attachmentCount=used;fi.pAttachments=compactViews;fi.width=op.texture->levelWidth(op.dstLevel);fi.height=op.texture->levelHeight(op.dstLevel);fi.layers=1;VkFramebuffer frame{};
     {Measure measure{objectNanos};auto found=std::find_if(frameCache.begin(),frameCache.end(),[&](auto c){return c.textures==targets && c.render==render && c.levels==levels;});
      if(found!=frameCache.end())frame=found->frame;
      else {check(CreateFramebuffer(device,&fi,nullptr,&frame));++reuseCounts.frames;++submitObjectCreates;++submitFrameCreates;
       try {if(frameCache.size()<frameCacheLimit)frameCache.push_back({targets,render,levels,frame});else frames.push_back(frame);}catch(...){DestroyFramebuffer(device,frame,nullptr);throw;}}
     }
     VkRenderPassBeginInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;ri.renderPass=render;ri.framebuffer=frame;
     ri.renderArea={{0,0},{op.areaWidth?op.areaWidth:op.texture->levelWidth(op.dstLevel),op.areaHeight?op.areaHeight:op.texture->levelHeight(op.dstLevel)}};ri.clearValueCount=used;ri.pClearValues=clears;
     CmdBeginRenderPass(cmd,&ri,VK_SUBPASS_CONTENTS_INLINE);CmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,op.strip?op.pipeline->stripPipeline:op.pipeline->pipeline);
     CmdSetViewport(cmd,0,1,&op.viewport);CmdSetScissor(cmd,0,1,&op.scissor);
     if(draw.descriptorCount) {
      VkDescriptorSetAllocateInfo sai{};sai.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;sai.descriptorPool=submitPool;sai.descriptorSetCount=1;sai.pSetLayouts=&op.pipeline->setLayout;
      VkDescriptorSet set{};{Measure measure{descriptorNanos};check(AllocateDescriptorSets(device,&sai,&set));}
      for(unsigned i=0;i<draw.descriptorCount;++i){const auto &d=draw.descriptors[i];
       VkWriteDescriptorSet write{};write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.dstBinding=d.binding;write.descriptorCount=1;write.descriptorType=(VkDescriptorType)d.type;
       VkDescriptorImageInfo image{};VkDescriptorBufferInfo buffer{};
       if(d.source==2){
        if(!neutralBuffer || d.offset || d.range>neutralBuffer->length)throw std::runtime_error("planned neutral buffer mismatch");
        buffer={neutralBuffer->buffer,0,d.range};write.pBufferInfo=&buffer;
       }else if(d.source==1){
        auto it=std::find_if(op.pipeline->descriptors.begin(),op.pipeline->descriptors.end(),[&](const Descriptor &layout){return layout.set==d.set && layout.binding==d.binding && layout.type==d.type && layout.stages==d.stages && layout.source==1 && layout.slot==d.slot && layout.range==d.range;});
        if(it==op.pipeline->descriptors.end() || d.slot>=op.pipeline->constantBlobs.size() || d.offset)throw std::runtime_error("planned constant blob mismatch");
        auto *b=op.pipeline->constantBlobs[d.slot];buffer={b->buffer,0,d.range};write.pBufferInfo=&buffer;
       }else if(d.type==VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT){
        if(!op.pipeline->fetchMask || !op.pipeline->orderedColorAccess || d.slot>=op.colorCount || !(op.pipeline->fetchMask&(1u<<d.slot)) || d.format!=(uint32_t)op.color(d.slot).texture->vkFormat || d.stages!=VK_SHADER_STAGE_FRAGMENT_BIT)throw std::runtime_error("planned input attachment mismatch");
        image={VK_NULL_HANDLE,targetViews[d.slot],VK_IMAGE_LAYOUT_GENERAL};write.pImageInfo=&image;
       }else if(d.type==VK_DESCRIPTOR_TYPE_SAMPLER){
        if(d.sampler>=resourceSlots)throw std::runtime_error("planned sampler slot");
        auto *sam=d.sampler==0 && !op.samplers[0]?op.sampler:op.samplers[d.sampler];if(!sam)throw std::runtime_error("planned unbound sampler");
        image.sampler=sam->sampler;write.pImageInfo=&image;
       }else if(d.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || d.type==VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE){
        if(d.slot>=resourceSlots || (d.sampler>=resourceSlots && d.sampler!=UINT32_MAX))throw std::runtime_error("planned sample slot");auto *t=d.slot==0 && !op.samples[0]?op.sourceTexture:op.samples[d.slot];auto *sam=d.sampler==UINT32_MAX?nullptr:d.sampler==0 && !op.samplers[0]?op.sampler:op.samplers[d.sampler];
        if(d.sampler==UINT32_MAX && (d.binding>=16 || !op.pipeline->bakedSamplers[d.binding]))throw std::runtime_error("planned constexpr sampler mismatch");
        if(!t || (d.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && d.sampler!=UINT32_MAX && !sam) || d.format!=(uint32_t)t->vkFormat)throw std::runtime_error("planned texture format mismatch");
        VkImageViewCreateInfo vi{};vi.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;vi.image=t->image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=(VkFormat)d.format;vi.components={(VkComponentSwizzle)d.components[0],(VkComponentSwizzle)d.components[1],(VkComponentSwizzle)d.components[2],(VkComponentSwizzle)d.components[3]};uint32_t base=op.sampleBase[d.slot],count=op.sampleCount[d.slot]?op.sampleCount[d.slot]:t->levels-base;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,base,count,0,1};
        VkImageView view{};
        {Measure measure{objectNanos};std::array<uint32_t,4> components{d.components[0],d.components[1],d.components[2],d.components[3]};
         auto found=std::find_if(viewCache.begin(),viewCache.end(),[&](auto c){return c.texture==t && c.format==vi.format && c.components==components && c.base==base && c.count==count;});
         if(found!=viewCache.end())view=found->view;
         else {check(CreateImageView(device,&vi,nullptr,&view));++reuseCounts.views;++submitObjectCreates;++submitViewCreates;
          try {if(viewCache.size()<viewCacheLimit)viewCache.push_back({t,vi.format,components,base,count,view});else sampleViews.push_back(view);}catch(...){DestroyImageView(device,view,nullptr);throw;}}
        }image={d.type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && sam?sam->sampler:VK_NULL_HANDLE,view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};write.pImageInfo=&image;
       }else {auto b=op.buffers[d.stages==1?0:1][d.slot];if(!b.buffer || !d.range || d.offset>((b.buffer->length+15)&~size_t(15)) || d.range>((b.buffer->length+15)&~size_t(15))-d.offset)throw std::runtime_error("planned buffer range mismatch");buffer={b.buffer->buffer,d.offset,d.range};write.pBufferInfo=&buffer;}
       {Measure measure{descriptorNanos};UpdateDescriptorSets(device,1,&write,0,nullptr);}
      }CmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,op.pipeline->layout,0,1,&set,0,nullptr);
     }
     for(unsigned i=0;i<draw.pushCount;++i){auto &w=draw.pushes[i];CmdPushConstants(cmd,op.pipeline->layout,w.stages,w.offset,w.size,w.bytes);}
     for(unsigned i=0;i<draw.vertexCount;++i){auto b=op.buffers[0][draw.vertexSlots[i]].buffer;VkDeviceSize offset=draw.vertexOffsets[i];CmdBindVertexBuffers(cmd,draw.vertexBindings[i],1,&b->buffer,&offset);}
     if(draw.indexed){CmdBindIndexBuffer(cmd,op.indices->buffer,draw.indexOffset,VK_INDEX_TYPE_UINT16);CmdDrawIndexed(cmd,draw.count,1,draw.first,0,0);}else CmdDraw(cmd,draw.count,1,draw.first,0);
     CmdEndRenderPass(cmd);
    } else if(op.texture && op.clearOnly && ((op.areaWidth && op.areaWidth!=op.texture->levelWidth(op.dstLevel)) || (op.areaHeight && op.areaHeight!=op.texture->levelHeight(op.dstLevel)))) {
     // Empty partial passes still execute Metal load/store over the chosen area.
     // A render pass CLEAR obeys renderArea; vkCmdClearColorImage cannot do so.
     Pipeline pass{};pass.options.format=0;pass.options.colorCount=op.colorCount;
     VkImageView views[8]{};VkClearValue clears[8]{};unsigned used=0;
     for(unsigned i=0;i<op.colorCount;++i){auto c=op.color(i);if(!c.texture)continue;
      if(c.load==1 && !c.texture->isDefined(c.level))throw std::runtime_error("load undefined partial empty attachment");
      if(i)pass.options.colors[i-1].format=c.texture->format;else pass.options.format=c.texture->format;
      views[used]=attachmentView(c.texture,c.level);memcpy(clears[used++].color.float32,c.clear,sizeof(c.clear));
     }
     auto render=renderPass(&pass,&op);try{emptyRenders.push_back(render);}catch(...){DestroyRenderPass(device,render,nullptr);throw;}
     VkFramebufferCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;fi.renderPass=render;fi.attachmentCount=used;fi.pAttachments=views;fi.width=op.texture->levelWidth(op.dstLevel);fi.height=op.texture->levelHeight(op.dstLevel);fi.layers=1;
     VkFramebuffer frame{};check(CreateFramebuffer(device,&fi,nullptr,&frame));try{frames.push_back(frame);}catch(...){DestroyFramebuffer(device,frame,nullptr);throw;}
     ++reuseCounts.frames;++submitObjectCreates;++submitFrameCreates;
     VkRenderPassBeginInfo ri{};ri.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;ri.renderPass=render;ri.framebuffer=frame;ri.renderArea={{0,0},{op.areaWidth?op.areaWidth:fi.width,op.areaHeight?op.areaHeight:fi.height}};ri.clearValueCount=used;ri.pClearValues=clears;
     CmdBeginRenderPass(cmd,&ri,VK_SUBPASS_CONTENTS_INLINE);CmdEndRenderPass(cmd);
     for(unsigned i=0;i<op.colorCount;++i){auto c=op.color(i);if(c.texture)c.texture->setDefined(c.level,c.store && (c.load==2 || (c.load==1 && c.texture->isDefined(c.level))));}
    } else if(op.texture && op.clearOnly) {
     for(unsigned i=0;i<op.colorCount;++i){auto c=op.color(i);auto *t=c.texture;if(!t)continue;
      if((op.areaWidth && op.areaWidth!=t->levelWidth(c.level)) || (op.areaHeight && op.areaHeight!=t->levelHeight(c.level)))throw std::runtime_error("partial render area on an empty pass");
      if(c.load==1 && !t->isDefined(c.level))throw std::runtime_error("load undefined empty attachment");
      if(c.load==2){VkClearColorValue clear{};memcpy(clear.float32,c.clear,sizeof(c.clear));VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,c.level,1,0,1};CmdClearColorImage(cmd,t->image,VK_IMAGE_LAYOUT_GENERAL,&clear,1,&range);}
      t->setDefined(c.level,c.store && (c.load==2 || (c.load==1 && t->isDefined(c.level))));
     }
    } else if(op.texture && op.sourceTexture) {
     if(!op.texture->isDefined(op.dstLevel) && (op.dstX || op.dstY || (op.width && op.width!=op.texture->levelWidth(op.dstLevel)) || (op.height && op.height!=op.texture->levelHeight(op.dstLevel)))){VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,op.dstLevel,1,0,1};CmdClearColorImage(cmd,op.texture->image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);
      barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);}
     op.texture->setDefined(op.dstLevel);
     VkImageCopy region{};region.srcOffset={(int32_t)op.srcX,(int32_t)op.srcY,0};region.dstOffset={(int32_t)op.dstX,(int32_t)op.dstY,0};region.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,op.srcLevel,0,1};region.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,op.dstLevel,0,1};region.extent={op.width?op.width:op.texture->levelWidth(op.dstLevel),op.height?op.height:op.texture->levelHeight(op.dstLevel),1};
     CmdCopyImage(cmd,op.sourceTexture->image,VK_IMAGE_LAYOUT_GENERAL,op.texture->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    } else if(op.texture && op.upload) {
     if(!op.texture->isDefined(op.dstLevel) && (op.dstX || op.dstY || (op.width && op.width!=op.texture->levelWidth(op.dstLevel)) || (op.height && op.height!=op.texture->levelHeight(op.dstLevel)))){VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,op.dstLevel,1,0,1};CmdClearColorImage(cmd,op.texture->image,VK_IMAGE_LAYOUT_GENERAL,&zero,1,&range);
      barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&barrier,0,nullptr,0,nullptr);}
     op.texture->setDefined(op.dstLevel);
     VkBufferImageCopy region{};region.bufferOffset=op.from;region.imageOffset={(int32_t)op.dstX,(int32_t)op.dstY,0};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,op.dstLevel,0,1};region.imageExtent={op.width?op.width:op.texture->levelWidth(op.dstLevel),op.height?op.height:op.texture->levelHeight(op.dstLevel),1};region.bufferRowLength=op.rowBytes?op.rowBytes/texelBytes(op.texture->format):0;
     CmdCopyBufferToImage(cmd,op.src->buffer,op.texture->image,VK_IMAGE_LAYOUT_GENERAL,1,&region);
    } else if(op.texture) {
     if(!op.texture->isDefined(op.srcLevel))throw std::runtime_error("undefined readback texture");
     VkBufferImageCopy region{};region.bufferOffset=op.to;region.imageOffset={(int32_t)op.srcX,(int32_t)op.srcY,0};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,op.srcLevel,0,1};region.imageExtent={op.width?op.width:op.texture->levelWidth(op.srcLevel),op.height?op.height:op.texture->levelHeight(op.srcLevel),1};region.bufferRowLength=op.rowBytes?op.rowBytes/texelBytes(op.texture->format):0;
     CmdCopyImageToBuffer(cmd,op.texture->image,VK_IMAGE_LAYOUT_GENERAL,op.dst->buffer,1,&region);
    } else if(op.src) {VkBufferCopy region{op.from,op.to,op.size};CmdCopyBuffer(cmd,op.src->buffer,op.dst->buffer,1,&region);}
    else CmdFillBuffer(cmd,op.dst->buffer,op.to,op.size,uint32_t(op.value)*0x01010101u);
    barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&barrier,0,nullptr,0,nullptr);
   }
   barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
   CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
   if(timestamps)CmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,timestamps,1);
   check(EndCommandBuffer(cmd));
   submitRecordNanos+=since(recordStart)-(objectNanos-recordObjects)-descriptorNanos;
   submitObjectsNanos+=objectNanos;submitDescriptorsNanos+=descriptorNanos;
#ifdef FUXI_MTL_HOST_TEST
   if(failBeforeQueue){failBeforeQueue=false;throw std::runtime_error("injected pre-submit failure");}
#endif
   VkSubmitInfo si{};si.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;si.commandBufferCount=1;si.pCommandBuffers=&cmd;
   // Any failure after attempting submission poisons ownership and avoids unsafe teardown.
   requireLive(); // A concurrent frontend refusal must cancel prepared work too.
   submitted=true;submitQueued=std::chrono::steady_clock::now();submitPrepareNanos+=(uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(submitQueued-submitStart).count();
#ifdef FUXI_MTL_HOST_TEST
   if(failQueueAttempt){failQueueAttempt=false;check(VK_ERROR_DEVICE_LOST);}
#endif
   check(QueueSubmit(queue,1,&si,fence));if(scheduled)scheduled();// Lavapipe can JIT a generic CA shader on its submission worker. The
   // offline qualifier may extend only its host wait; production stays 2 s.
#if defined(FUXI_MTL_HOST_TEST) && defined(FUXI_MTL_HOST_FENCE_NS)
   check(WaitForFences(device,1,&fence,VK_TRUE,FUXI_MTL_HOST_FENCE_NS));
#else
   check(WaitForFences(device,1,&fence,VK_TRUE,2000000000ULL));
#endif
   submitted=false;completed=true;submitWaitNanos+=since(submitQueued);submitDone=std::chrono::steady_clock::now();
   if(timestamps){uint64_t ticks[2]{};check(GetQueryPoolResults(device,timestamps,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT));
    uint64_t mask=timestampBits>=64?UINT64_MAX:(uint64_t(1)<<timestampBits)-1;
    gpuNanos.fetch_add((uint64_t)(double((ticks[1]-ticks[0])&mask)*timestampPeriod));++gpuSamples;
   }
   auto invalidateStart=std::chrono::steady_clock::now();
   for(auto *t:textures)if(t->mapped && !coherent(t->memoryType)) {VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=t->memory;r.size=VK_WHOLE_SIZE;++invalidateCalls;check(InvalidateMappedMemoryRanges(device,1,&r));}
   for(auto *b:buffers)if(!coherent(b->memoryType)) {VkMappedMemoryRange r{};r.sType=VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;r.memory=b->memory;r.size=VK_WHOLE_SIZE;++invalidateCalls;check(InvalidateMappedMemoryRanges(device,1,&r));}
   submitInvalidateNanos+=since(invalidateStart);
   cleanup();
#ifdef FUXI_MTL_HOST_TEST
   if(!reuseSubmitObjects){clearSubmitObjects();clearFrames();clearViews();}
#endif
   ++submits;submitRetireNanos+=since(submitDone);
  } catch(...) {if(submitted) poisoned=true;else {if(!completed)for(auto old:oldTextures){old.texture->layout=old.layout;old.texture->defined=old.defined;old.texture->definedLevels=old.definedLevels;}clearSubmitObjects();cleanup();}
   try{throw;}catch(const std::exception &e){if(diagnosticOp<ops.size() && !ops[diagnosticOp].diagnostic.empty())throw std::runtime_error(std::string(e.what())+" desc="+ops[diagnosticOp].diagnostic);throw;}
  }
 }
};
}
