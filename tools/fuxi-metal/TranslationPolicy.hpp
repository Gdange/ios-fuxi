/* Original opt-in translation policy; duplicate/malformed boot keys disable. */
#pragma once
#include "ClientGate.hpp"
#include <cstdlib>
namespace fuxi {
inline bool translationOption(const char *args,const char *key,bool *present=nullptr) {
 unsigned count=0;bool enabled=false;size_t length=strlen(key);
 if(args)for(const char *p=args;*p;){
  p+=strspn(p," \t\r\n");size_t n=strcspn(p," \t\r\n");
  if(n>=length && !strncmp(p,key,length)){++count;enabled=n==length+1 && p[length]=='1';}p+=n;
 }
 if(present)*present=count!=0;return count==1 && enabled;
}
struct TranslationPolicy {bool stages=false,trace=false,async=false;};
inline TranslationPolicy translationPolicy(const char *args) {
 return {translationOption(args,"fuxigpustages="),translationOption(args,"fuxigputrace="),translationOption(args,"fuxigpuasync=")};
}
inline TranslationPolicy systemTranslationPolicy() {
 static const auto policy=[] {
  char args[4096]={};
#ifdef FUXI_MTL_HOST_TEST
  const char *test=getenv("FUXI_MTL_TEST_BOOTARGS");if(test)snprintf(args,sizeof(args),"%s",test);
#else
  systemString("kern.bootargs",args,sizeof(args));
#endif
  bool stages=false,trace=false,async=false;
  TranslationPolicy p{translationOption(args,"fuxigpustages=",&stages),translationOption(args,"fuxigputrace=",&trace),translationOption(args,"fuxigpuasync=",&async)};
  // Explicit boot keys, including =0/invalid/duplicates, take precedence.
  if(!stages){const char *v=getenv("FUXI_MTL_STAGE_CACHE");p.stages=v && !strcmp(v,"1");}
  if(!trace){const char *v=getenv("FUXI_MTL_TRANSLATION_TRACE");p.trace=v && !strcmp(v,"1");}
  if(!async){const char *v=getenv("FUXI_MTL_ASYNC_PIPELINES");p.async=v && !strcmp(v,"1");}
  return p;
 }();return policy;
}
}
