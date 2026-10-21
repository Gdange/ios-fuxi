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
 assert(!fuxi::validFetchContract(true,0,1) && !fuxi::validFetchContract(true,1,2));
 assert(!fuxi::validFetchContract(true,2,1) && !fuxi::validFetchContract(true,1,UINT32_MAX));
 assert(fuxi::bindingRange(0,16) && fuxi::bindingRange(16,0));
 assert(!fuxi::bindingRange(16,1) && !fuxi::bindingRange(std::numeric_limits<size_t>::max(),2));
 assert(fuxi::bindingOffset(true,false,128,127));
 assert(!fuxi::bindingOffset(true,false,128,128) && !fuxi::bindingOffset(false,false,128,0));
 assert(!fuxi::bindingOffset(true,true,128,0));
 assert(fuxi::neutralDepth(7,false) && !fuxi::neutralDepth(7,true) && !fuxi::neutralDepth(1,false));
 assert(fuxi::neutralStencil(7,0,0,0) && !fuxi::neutralStencil(7,0,0,1));
 fuxi::PipelineCache cache;cache.close();cache.open(VK_NULL_HANDLE,create,destroy,size);
 auto first=cache.get();assert(first && cache.get()==first && made==1);
 for(unsigned i=0;i<fuxi::PipelineCache::maxPairs-1;++i){cache.finishedPair();assert(cache.get()==first);}
 cache.finishedPair();assert(live.empty());auto second=cache.get();assert(second!=first);
 blobSize=fuxi::PipelineCache::maxBytes+1;cache.finishedPair();assert(live.empty());
 blobSize=32;cache.get();failSize=true;cache.finishedPair();assert(live.empty());failSize=false;
 failCreate=true;assert(!cache.get());cache.finishedPair();assert(live.empty());failCreate=false;
 assert(cache.get());cache.close();cache.close();assert(live.empty() && made==dropped);
 puts("PASS: production CA bounds/neutral-state controls; cache reuse, generation/size retirement, query/create failure and teardown; no submissions");
}
