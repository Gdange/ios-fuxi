/* Fuxi startup policy; design observations credited in metal-m9m.md. */
#pragma once
#include <cstdlib>
#include <cstring>
namespace fuxi {
template<class Environment> void setupTurnip(Environment env) {
 // Preserve an explicit diagnostic override, including an empty one.
 env("TU_DEBUG","sysmem,nocb",0);
}
inline void setupTurnip(){setupTurnip([](const char *key,const char *value,int overwrite){return setenv(key,value,overwrite);});}
template<class Environment,class Preference>
void setupMetalClient(bool admitted,const char *process,Environment env,Preference preference) {
 if(!admitted)return;
 setupTurnip(env);
 if(!strcmp(process,"backboardd")){
  env("CA_DISABLE_FRAMEBUFFER_COMPRESSION","1",1);
  preference("CAEnableFramebufferCompression");
  preference("CAEnableDeepFramebuffer");
 }
 preference("CAEnableIOSurfaceCompression");
}
}
