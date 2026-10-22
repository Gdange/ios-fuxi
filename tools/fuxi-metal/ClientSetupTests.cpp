/* Startup policy with injected side effects: never writes real preferences. */
#include "ClientSetup.hpp"
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
int main(){
 std::map<std::string,std::string> values;std::vector<std::string> preferences;
 auto env=[&](const char *key,const char *value,int overwrite){if(overwrite || !values.count(key))values[key]=value;return 0;};
 auto pref=[&](const char *key){preferences.emplace_back(key);};
 for(const char *name:{"backboardd","SpringBoard","smoke"})fuxi::setupMetalClient(false,name,env,pref);
 assert(values.empty() && preferences.empty());
 fuxi::setupMetalClient(true,"smoke",env,pref);
 assert(values.size()==1 && values.at("TU_DEBUG")=="sysmem,nocb");
 assert(preferences==std::vector<std::string>{"CAEnableIOSurfaceCompression"});
 values["TU_DEBUG"]="override";values["CA_DISABLE_FRAMEBUFFER_COMPRESSION"]="0";preferences.clear();
 fuxi::setupMetalClient(true,"backboardd",env,pref);
 assert(values.at("TU_DEBUG")=="override" && values.at("CA_DISABLE_FRAMEBUFFER_COMPRESSION")=="1");
 assert((preferences==std::vector<std::string>{"CAEnableFramebufferCompression","CAEnableDeepFramebuffer","CAEnableIOSurfaceCompression"}));
 values["TU_DEBUG"]="";fuxi::setupTurnip(env);assert(values.at("TU_DEBUG").empty());
 puts("PASS: admitted-only startup, sysmem/nocb default and override, render-server/client compression policy via hooks");
}
