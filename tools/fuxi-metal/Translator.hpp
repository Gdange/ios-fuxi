/* Original dynamic boundary. The translator and all its dependencies stay external. */
#pragma once
#include <cstddef>
#include "ConstantLayout.hpp"
#include "CAState.hpp"
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
// ABI 6. All ordinals in the input are public Metal values; outputs are Vulkan values.
struct Constant {uint32_t index{},type{},size{};uint8_t bytes[16]{};char name[128]{};};
struct Attribute {uint32_t format{},offset{},buffer{};};
struct CompileOptions {
 uint32_t format=80,blend=0,srcRGB=1,dstRGB=0,rgbOp=0,srcAlpha=1,dstAlpha=0,alphaOp=0,mask=15,strip=0;
 Attribute attributes[8]{};uint32_t strides[4]{};uint32_t constantCount[2]{};Constant constants[2][maxFunctionConstants]{};
 // Actual VkDevice enables, supplied by Backend; never inferred from support.
 uint32_t orderedColorAccess{};
};
struct Descriptor {uint32_t binding{},type{},stages{},slot{},sampler{},format{},components[4]{};uint64_t offset{},range{};uint32_t set{},source{};};
struct VertexBinding {uint32_t binding{},slot{},stride{};};
struct VertexAttribute {uint32_t location{},binding{},format{},offset{};};
struct BoundBuffer {uint64_t length{},offset{};uint32_t bytes{};};
struct DrawInput {BoundBuffer buffers[2][resourceSlots]{};uint32_t formats[resourceSlots]{},filters[resourceSlots]{};uint32_t count{},start{},indexed{},strip{};uint64_t indexLength{},indexOffset{};};
struct Push {uint32_t offset{},size{},stages{};uint8_t bytes[128]{};};
struct DrawOutput {uint32_t descriptorCount{},pushCount{},vertexCount{},indexed{},count{},first{};uint64_t indexOffset{};Descriptor descriptors[16]{};Push pushes[2]{};uint32_t vertexBindings[4]{},vertexSlots[4]{};uint64_t vertexOffsets[4]{};};
struct Specialization {uint32_t id{},offset{},size{};};
struct Range {uint32_t offset{},size{},stages{};};
struct BakedSampler {uint32_t enabled{},mag{},min{},mip{},address[3]{};float bias{},minLod{},maxLod{};uint32_t border{};};
struct Blob {const uint8_t *bytes{};size_t length{};uint8_t hash[32]{};};
struct Translation {
 void *handle{}; const uint32_t *vertex{}; size_t vertexCount{}; const char *vertexEntry{};
 const uint32_t *fragment{}; size_t fragmentCount{}; const char *fragmentEntry{};
 uint32_t pushOffset{},pushSize{},pushStages{};uint8_t pushBytes[128]{};uint32_t descriptorCount{};Descriptor descriptors[16]{};uint32_t specCount{},specSize{};Specialization specs[32]{};uint8_t specData[256]{};
 uint32_t bindingCount{},attributeCount{};VertexBinding bindings[4]{};VertexAttribute attributes[8]{};uint32_t format{},blend{},mask{},factors[6]{};uint32_t rangeCount{};Range ranges[2]{};uint32_t fetchMask{},orderedColorAccess{};uint32_t blobCount{};Blob blobs[2]{};
};
class Translator {
 void *dylib{};
public:
 void *(*open)(const uint8_t *,size_t,char *,size_t){};
 void (*close)(void *){};
 int (*function)(void *,const char *,char *,size_t){};
 int (*compile)(void *,const char *,void *,const char *,void *,uint64_t,uint64_t,const CompileOptions *,Translation *,char *,size_t){};
 int (*draw)(void *,const DrawInput *,DrawOutput *,char *,size_t){};
 int (*variant)(void *,const DrawInput *,Translation *,char *,size_t){};
 int (*sampler)(void *,uint32_t,BakedSampler *,char *,size_t){};
 int (*identity)(void *,const char *,void *,const char *,const CompileOptions *,uint8_t *,char *,size_t){};
 int (*product_hash)(void *,uint8_t *){};
 void (*free)(void *){};
 Translator() {
  dylib=dlopen(FUXI_MTL_TRANSLATOR,RTLD_NOW|RTLD_LOCAL);
  if(!dylib)throw std::runtime_error("dlopen external metal2vulkan failed");
  auto abi=(uint32_t (*)())dlsym(dylib,"fuxi_m2v_abi");
  if(!abi || abi()!=6) {dlclose(dylib);throw std::runtime_error("metal2vulkan C ABI mismatch");}
#define LOAD(n) n=(decltype(n))dlsym(dylib,"fuxi_m2v_" #n);if(!n) {dlclose(dylib);throw std::runtime_error("missing translator " #n);}
  LOAD(open) LOAD(close) LOAD(function) LOAD(compile) LOAD(draw) LOAD(variant) LOAD(sampler) LOAD(identity) LOAD(product_hash) LOAD(free)
#undef LOAD
 }
 ~Translator() {if(dylib)dlclose(dylib);}
 Translator(const Translator&)=delete;Translator& operator=(const Translator&)=delete;
 static Translator &get() {static Translator translator;return translator;}
 static bool cacheIdentity(uint8_t *out) noexcept {
  try {
   auto &tr=get();auto fn=(int (*)(uint8_t *))dlsym(tr.dylib,"fuxi_m2v_cache_identity");
   return fn && fn(out)==1;
  }catch(...){return false;} // Older/unavailable bridges keep the memory-only path.
 }
};
}
