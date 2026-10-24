#include <string>
#include <initializer_list>
#define FUXI_MTL_HOST_TEST
#include "TranslationPolicy.hpp"
#include <cassert>
int main(int argc,char **argv){
 if(argc==2){auto p=fuxi::systemTranslationPolicy();assert(p.stages==(argv[1][0]=='1') && p.trace==(argv[1][1]=='1') && p.async==(argv[1][2]=='1'));return 0;}
 using namespace fuxi;
 assert(!translationPolicy(nullptr).stages && !translationPolicy("").trace);
 auto p=translationPolicy("fuxigpustages=1 fuxigputrace=1 fuxigpuasync=1");assert(p.stages && p.trace && p.async);
 for(auto key:{"fuxigpustages","fuxigputrace","fuxigpuasync"}){
  auto check=[&](const std::string &s){return translationOption(s.c_str(),(std::string(key)+"=").c_str());};
  assert(check(std::string("other=1\t")+key+"=1\r\n"));
  for(auto suffix:{"0","2","","11","yes"})assert(!check(std::string(key)+"="+suffix));
  assert(!check(std::string(key)+"=1 "+key+"=1"));assert(!check(std::string(key)+"=1 "+key+"=0"));
  assert(!check(std::string("prefix")+key+"=1"));
 }
 puts("PASS: translation defaults, exact tokens, duplicate/malformed settings fail closed");
}
