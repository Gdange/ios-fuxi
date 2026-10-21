/* Original process-only Vulkan cache, informed by NativeMetal's cache layers.
 * No serialized driver code or Apple-derived input is read from/written to disk.
 * All access is under Backend::mutex. Cache failure only loses acceleration. */
#pragma once
#include <vulkan/vulkan.h>
#include <cstddef>
namespace fuxi {
class PipelineCache {
 VkDevice device{};VkPipelineCache cache{};
 PFN_vkCreatePipelineCache create{};PFN_vkDestroyPipelineCache destroy{};PFN_vkGetPipelineCacheData data{};
 unsigned pairs{};
public:
 static constexpr size_t maxBytes=16*1024*1024;
 static constexpr unsigned maxPairs=8;
 void open(VkDevice d,PFN_vkCreatePipelineCache c,PFN_vkDestroyPipelineCache x,PFN_vkGetPipelineCacheData g) {
  device=d;create=c;destroy=x;data=g;
 }
 VkPipelineCache get() {
  if(!cache) {VkPipelineCacheCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
   if(create(device,&ci,nullptr,&cache)!=VK_SUCCESS)cache=VK_NULL_HANDLE;
  }
  return cache;
 }
 void finishedPair() {
  if(!cache)return;
  size_t bytes=0;
  // Query only the size. Retire between creations; existing VkPipelines don't
  // depend on the cache. This bounds retained generations, not driver scratch.
  if(++pairs>=maxPairs || data(device,cache,&bytes,nullptr)!=VK_SUCCESS || bytes>maxBytes)close();
 }
 void close() {if(cache)destroy(device,cache,nullptr);cache=VK_NULL_HANDLE;pairs=0;}
 PipelineCache()=default;
 PipelineCache(const PipelineCache&)=delete;PipelineCache& operator=(const PipelineCache&)=delete;
};
}
