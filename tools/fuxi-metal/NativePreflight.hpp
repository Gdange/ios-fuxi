/* Original MIT query/close ordering, portable for tests. */
#pragma once
#include <cstddef>
#include "../../xnu/fuxi-adreno/NativeAdrenoABI.h"
namespace fuxi {
template<class Open,class Query,class Close>
bool nativeImportPreflight(Open open,Query query,Close close){
 uint32_t connection=0;
 if(!open(&connection))return false;
 NativeAdrenoAbiParams params={};size_t bytes=sizeof(params);
 bool ok=query(connection,&params,&bytes) && bytes==sizeof(params) &&
  params.abi_version==NativeAdrenoAbiVersion && (params.flags&NativeAdrenoParamsHostImport);
 // Release even a refused/truncated/version-mismatched query, before returning.
 bool closed=close(connection);return ok && closed;
}
}
