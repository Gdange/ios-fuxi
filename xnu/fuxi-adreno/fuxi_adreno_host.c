/* HostImport's private pmap queries belong to Mach, not IOKit or BSD. */
#include <pexpert/arm64/board_config.h>
#if defined(FUXI)
#include <stdbool.h>
#include <vm/pmap.h>
#include "../../bsd/dev/fuxi_gpu_abi.h"

bool fuxi_adreno_host_page_ok(uint64_t pa)
{
 ppnum_t page=(ppnum_t)(pa>>14); /* NativeAdrenoABI's fixed 16K host page. */
 if (!pmap_valid_page(page)) return false;
 unsigned cache=pmap_cache_attributes(page)&VM_WIMG_MASK;
 return cache==VM_WIMG_DEFAULT || cache==VM_WIMG_WCOMB;
}
#endif
