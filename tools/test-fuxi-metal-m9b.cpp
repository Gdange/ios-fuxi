/* Linux source controls: production validation/cache lifetime, no submissions. */
#define VK_NO_PROTOTYPES
#include "fuxi-metal/PipelineCache.hpp"
#include "fuxi-metal/CAState.hpp"
#include "fuxi-metal/FramebufferFetch.hpp"
#include <cassert>
#include <cstdio>
#include <limits>
#include <set>
static unsigned made,dropped;
static bool failCreate,failSize;
static size_t blobSize=32;
static std::set<VkPipelineCache> live;
static VkResult VKAPI_CALL create(VkDevice,const VkPipelineCacheCreateInfo *ci,const VkAllocationCallbacks *,VkPipelineCache *out) {
 assert(!ci->initialDataSize && !ci->pInitialData && !ci->pNext);
 if(failCreate)return VK_ERROR_OUT_OF_HOST_MEMORY;
 *out=(VkPipelineCache)(uintptr_t)(++made);live.insert(*out);return VK_SUCCESS;
}
static void VKAPI_CALL destroy(VkDevice,VkPipelineCache c,const VkAllocationCallbacks *) {assert(live.erase(c)==1);++dropped;}
static VkResult VKAPI_CALL size(VkDevice,VkPipelineCache c,size_t *n,void *bytes) {
 assert(live.count(c) && !bytes);*n=blobSize;return failSize?VK_ERROR_DEVICE_LOST:VK_SUCCESS;
}
int main() {
 assert(fuxi::validFetchContract(false,0,0) && fuxi::validFetchContract(true,1,0) && fuxi::validFetchContract(true,1,1));
 assert(!fuxi::validFetchContract(false,0,1) && !fuxi::validFetchContract(false,1,1));
 assert(!fuxi::validFetchContract(true,0,1) && fuxi::validFetchContract(true,1,2));
 assert(!fuxi::validFetchContract(true,2,1) && !fuxi::validFetchContract(true,1,UINT32_MAX));
 assert(fuxi::bindingRange(0,16) && fuxi::bindingRange(16,0));
 assert(!fuxi::bindingRange(16,1) && !fuxi::bindingRange(std::numeric_limits<size_t>::max(),2));
 assert(fuxi::bindingOffset(true,false,128,127));
 assert(!fuxi::bindingOffset(true,false,128,128) && !fuxi::bindingOffset(false,false,128,0));
 assert(!fuxi::bindingOffset(true,true,128,0));
 assert(fuxi::neutralDepth(7,false) && !fuxi::neutralDepth(7,true) && !fuxi::neutralDepth(1,false));
 assert(fuxi::neutralStencil(7,0,0,0) && !fuxi::neutralStencil(7,0,0,1));
 fuxi::PipelineCache cache;cache.close();
 VkPhysicalDeviceProperties props{};VkPhysicalDeviceDriverProperties driver{};VkPhysicalDeviceIDProperties id{};
 auto open=[&](){cache.open(VK_NULL_HANDLE,create,destroy,size,props,driver,id,nullptr,"cache-test");};
 open();auto first=cache.get();assert(first && cache.get()==first && made==1);
 // Persistent caches keep a healthy generation; pair counts no longer retire it.
 for(unsigned i=0;i<128;++i){cache.finishedPair();assert(cache.get()==first && made==1);}
 blobSize=fuxi::PipelineCache::maxBytes+1;cache.finishedPair();
 auto second=cache.get();assert(second && second!=first && live.size()==1);
 blobSize=32;failSize=true;cache.finishedPair();assert(cache.get()!=second && live.size()==1);failSize=false;
 failCreate=true;blobSize=fuxi::PipelineCache::maxBytes+1;cache.finishedPair();assert(!cache.get() && live.empty());
 cache.finishedPair();cache.close();failCreate=false;blobSize=32;open();
 assert(cache.get());cache.close();cache.close();assert(live.empty() && made==dropped);
 puts("PASS: production CA bounds/neutral-state controls; persistent cache reuse, size/query recreation, create failure and teardown; no submissions");
}
