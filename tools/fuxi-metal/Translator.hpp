/* Original dynamic boundary. The translator and all its dependencies stay external. */
#pragma once
#include <cstddef>
#include <cstdint>
#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <chrono>
#include <cstdio>
#ifndef FUXI_MTL_TRANSLATOR
#define FUXI_MTL_TRANSLATOR "/usr/local/lib/libmetal2vulkan.dylib"
#endif
namespace fuxi {
struct Descriptor {uint32_t binding{},type{},stages{};};
struct Specialization {uint32_t id{},offset{},size{};};
struct Translation {
 void *handle{}; const uint32_t *vertex{}; size_t vertexCount{}; const char *vertexEntry{};
 const uint32_t *fragment{}; size_t fragmentCount{}; const char *fragmentEntry{};
 uint32_t pushOffset{},pushSize{},pushStages{};uint8_t pushBytes[128]{};uint32_t descriptorCount{};Descriptor descriptors[4]{};uint32_t specCount{},specSize{};Specialization specs[32]{};uint8_t specData[256]{};
};
class Translator {
 void *dylib{};
public:
 void *(*open)(const uint8_t *,size_t,char *,size_t){};
 void (*close)(void *){};
 int (*function)(void *,const char *,char *,size_t){};
 int (*compile)(void *,const char *,void *,const char *,void *,uint64_t,uint64_t,Translation *,char *,size_t){};
 void (*free)(void *){};
 Translator() {
  dylib=dlopen(FUXI_MTL_TRANSLATOR,RTLD_NOW|RTLD_LOCAL);
  if(!dylib)throw std::runtime_error("dlopen external metal2vulkan failed");
  auto abi=(uint32_t (*)())dlsym(dylib,"fuxi_m2v_abi");
  if(!abi || abi()!=2) {dlclose(dylib);throw std::runtime_error("metal2vulkan C ABI mismatch");}
#define LOAD(n) n=(decltype(n))dlsym(dylib,"fuxi_m2v_" #n);if(!n) {dlclose(dylib);throw std::runtime_error("missing translator " #n);}
  LOAD(open) LOAD(close) LOAD(function) LOAD(compile) LOAD(free)
#undef LOAD
 }
 ~Translator() {if(dylib)dlclose(dylib);}
 Translator(const Translator&)=delete;Translator& operator=(const Translator&)=delete;
 static Translator &get() {static Translator translator;return translator;}
};
}
