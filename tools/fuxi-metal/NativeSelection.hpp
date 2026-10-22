/* Backend selection is independent of admission. Unknown/duplicate keys deny. */
#pragma once
#include <cstring>
namespace fuxi {
enum class MetalBackend { fuxi, native, invalid };
inline MetalBackend selectMetalBackend(const char *args) {
 if(!args)return MetalBackend::fuxi;
 unsigned keys=0;MetalBackend backend=MetalBackend::fuxi;
 for(const char *p=args;*p;){
  p+=strspn(p," \t\r\n");size_t n=strcspn(p," \t\r\n");
  if(n>=10 && !strncmp(p,"fuximetal=",10)){
   ++keys;
   backend=n==16 && !strncmp(p+10,"native",6)?MetalBackend::native:
    n==14 && !strncmp(p+10,"fuxi",4)?MetalBackend::fuxi:MetalBackend::invalid;
  }
  p+=n;
 }
 return keys>1?MetalBackend::invalid:backend;
}
}
