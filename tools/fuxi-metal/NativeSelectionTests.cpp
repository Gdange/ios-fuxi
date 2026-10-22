#include "NativeSelection.hpp"
#include "ClientGate.hpp"
#include "NativePreflight.hpp"
#include <cassert>
#include <string>
int main(){
 using B=fuxi::MetalBackend;
 assert(fuxi::selectMetalBackend(nullptr)==B::fuxi);
 assert(fuxi::selectMetalBackend("")==B::fuxi);
 assert(fuxi::selectMetalBackend("fuximetal=fuxi")==B::fuxi);
 assert(fuxi::selectMetalBackend("other=1\tfuximetal=native\nfuxibbgpu=2")==B::native);
 assert(fuxi::selectMetalBackend("xfuximetal=native")==B::fuxi);
 for(const char *args:{"fuximetal=", "fuximetal=Native", "fuximetal=native-extra", "fuximetal=fuxi fuximetal=native", "fuximetal=native fuximetal=native"})
  assert(fuxi::selectMetalBackend(args)==B::invalid);
 // Selection never provides either admission key, and cannot enable SpringBoard.
 for(const char *args:{"fuximetal=native", "fuximetal=native fuxibbgpu=1", "fuximetal=native fuxibbgpu=2"}){
  std::string config="backboardd\nSpringBoard\n";config.push_back(0);
  assert(fuxi::parseClients(config.data(),config.size()-1,"SpringBoard",args)==fuxi::ClientMode::deny);
  config="census backboardd\n";config.push_back(0);
  assert(fuxi::parseClients(config.data(),config.size()-1,"backboardd",args)==fuxi::ClientMode::deny);
 }
 char noBoot[]="backboardd\n";
 assert(fuxi::parseClients(noBoot,sizeof(noBoot)-1,"backboardd","fuximetal=native")==fuxi::ClientMode::deny);
 for(unsigned failure=0;failure<7;++failure){
  unsigned opens=0,queries=0,closes=0;
  bool ok=fuxi::nativeImportPreflight(
   [&](uint32_t *connection){++opens;*connection=42;return failure!=1;},
   [&](uint32_t connection,NativeAdrenoAbiParams *params,size_t *bytes){
    assert(connection==42 && !closes);++queries;params->abi_version=failure==3?1:2;
    params->flags=failure==4?0:NativeAdrenoParamsHostImport;if(failure==5)--*bytes;return failure!=2;
   },
   [&](uint32_t connection){assert(connection==42 && queries);++closes;return failure!=6;});
  assert(opens==1 && ok==(failure==0) && queries==(failure==1?0u:1u) && closes==queries);
 }
 puts("PASS: backend selection defaults, exact tokens, duplicates, and independent two-key/SpringBoard denial");
}
