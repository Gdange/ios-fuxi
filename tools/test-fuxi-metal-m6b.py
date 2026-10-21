#!/usr/bin/env python3
"""Build our bridge against clean pinned external sources; test CI AIR on Linux Vulkan.
Inputs and build products stay outside tracked source. No device or Apple tools.
"""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
# Inspect the actual Vulkan calls as well as their pixel result: the range's
# nonzero offset and plan's bytes must reach each draw, including a cache hit.
PROXY = r'''
#include <cstdio>
static PFN_vkCreateDescriptorSetLayout makeSet;
static PFN_vkCreatePipelineLayout makeLayout;
static PFN_vkCreateDescriptorPool makePool;
static PFN_vkAllocateDescriptorSets allocateSet;
static PFN_vkUpdateDescriptorSets updateSet;
static PFN_vkCmdBindDescriptorSets bindSet;
static PFN_vkCmdPushConstants push;
static PFN_vkCmdDraw draw;
static bool bound,pushed,written;
static VkResult setLayout(VkDevice d,const VkDescriptorSetLayoutCreateInfo *c,const VkAllocationCallbacks *a,VkDescriptorSetLayout *o) {
 assert(c->bindingCount==1 && !c->flags);
 const auto &b=c->pBindings[0];assert(b.binding==0 && b.descriptorCount==1 && b.descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && b.stageFlags==16 && !b.pImmutableSamplers);
 return makeSet(d,c,a,o);
}
static VkResult layout(VkDevice d,const VkPipelineLayoutCreateInfo *c,const VkAllocationCallbacks *a,VkPipelineLayout *o) {
 assert(c->setLayoutCount==1 && c->pushConstantRangeCount==1);
 const auto &r=c->pPushConstantRanges[0];assert(r.offset==64 && r.size==4 && r.stageFlags==16);
 return makeLayout(d,c,a,o);
}
static VkResult pool(VkDevice d,const VkDescriptorPoolCreateInfo *c,const VkAllocationCallbacks *a,VkDescriptorPool *o) {
 assert(c->maxSets==1 && c->poolSizeCount==1 && c->pPoolSizes[0].type==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER && c->pPoolSizes[0].descriptorCount==1);
 return makePool(d,c,a,o);
}
static VkResult allocated(VkDevice d,const VkDescriptorSetAllocateInfo *c,VkDescriptorSet *o) {
 assert(c->descriptorSetCount==1 && c->pSetLayouts[0]);return allocateSet(d,c,o);
}
static void updated(VkDevice d,uint32_t n,const VkWriteDescriptorSet *w,uint32_t copies,const VkCopyDescriptorSet *c) {
 assert(n==1 && !copies && w->dstBinding==0 && !w->dstArrayElement && w->descriptorCount==1 && w->descriptorType==VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
 assert(w->pImageInfo->sampler && w->pImageInfo->imageView && w->pImageInfo->imageLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
 written=true;updateSet(d,n,w,copies,c);
}
static void boundSet(VkCommandBuffer c,VkPipelineBindPoint b,VkPipelineLayout l,uint32_t first,uint32_t n,const VkDescriptorSet *s,uint32_t dynamic,const uint32_t *o) {
 assert(written && first==0 && n==1 && !dynamic);bound=true;bindSet(c,b,l,first,n,s,dynamic,o);
}
static void pushedBytes(VkCommandBuffer c,VkPipelineLayout l,VkShaderStageFlags stages,uint32_t offset,uint32_t size,const void *p) {
 uint32_t word=0;assert(size==4);memcpy(&word,p,4);assert(stages==16 && offset==64 && word==0x2222);
 pushed=true;push(c,l,stages,offset,size,p);
}
static void drawn(VkCommandBuffer c,uint32_t vertices,uint32_t instances,uint32_t first,uint32_t base) {
 assert(bound && pushed && written && vertices==6 && instances==1 && !first && !base);
 bound=pushed=written=false;draw(c,vertices,instances,first,base);
 puts("PASS: Vulkan descriptor set0 binding0 + fragment push64:4=0x2222 at draw");
}
'''
WRAPS = '''
 WRAP(CreateDescriptorSetLayout,makeSet,setLayout);WRAP(CreatePipelineLayout,makeLayout,layout);
 WRAP(CreateDescriptorPool,makePool,pool);WRAP(AllocateDescriptorSets,allocateSet,allocated);
 WRAP(UpdateDescriptorSets,updateSet,updated);WRAP(CmdBindDescriptorSets,bindSet,boundSet);
 WRAP(CmdPushConstants,push,pushedBytes);WRAP(CmdDraw,draw,drawn);
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--source', type=Path, required=True, help='clean external metal2vulkan git checkout at recipe pin')
    ap.add_argument('--build', type=Path, default=ROOT / 'deps/metal2vulkan-host-bridge')
    ap.add_argument('--metallib', type=Path, required=True)
    ap.add_argument('--sums', type=Path, required=True, help='CI SHA256SUMS containing metallib')
    ap.add_argument('--headers', type=Path, required=True)
    ap.add_argument('--icd', type=Path, default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    a = ap.parse_args()
    pin = re.search(r'^pin=([0-9a-f]{40})$', (ROOT / 'tools/build-fuxi-metal-translator.sh').read_text(), re.M)[1]
    def git(*args):
        return subprocess.check_output(['git', '-C', str(a.source), *args], text=True).strip()
    assert git('rev-parse', 'HEAD') == pin, 'external source pin mismatch'
    assert not git('status', '--porcelain', '--untracked-files=no'), 'dirty external sources'
    expected = [line.split()[0] for line in a.sums.read_text().splitlines() if line.split()[-1] == a.metallib.name]
    assert expected == [hashlib.sha256(a.metallib.read_bytes()).hexdigest()], 'CI metallib SHA-256 mismatch'
    print('PASS: external revision and CI metallib SHA-256', flush=True)
    build = a.build.resolve();(build / 'src').mkdir(parents=True, exist_ok=True)
    (build / 'src/lib.rs').write_bytes((ROOT / 'tools/fuxi-metal2vulkan-bridge.rs').read_bytes())
    # TOML quoted paths use JSON escaping, never shell interpolation.
    import json
    source = json.dumps(str(a.source.resolve() / 'crates/metal2vulkan'))
    (build / 'Cargo.toml').write_text('''[package]
name = "fuxi-metal2vulkan-bridge"
version = "0.1.0"
edition = "2021"
[lib]
name = "metal2vulkan"
crate-type = ["cdylib"]
[dependencies]
metal2vulkan = { path = ''' + source + ''', features = ["ash"] }
ash = { version = "0.38", default-features = false, features = ["std"] }
''')
    for command in ('test', 'build'):
        subprocess.run(['cargo', '+1.98.0', command, '--manifest-path', str(build / 'Cargo.toml')], check=True)
    spec = importlib.util.spec_from_file_location('vk_smoke', ROOT / 'tools/test-fuxi-vk-smoke.py')
    smoke = importlib.util.module_from_spec(spec);spec.loader.exec_module(smoke)
    with tempfile.TemporaryDirectory() as tmp:
        d = Path(tmp)
        proxy = smoke.PROXY.replace('static bool compute;', 'static bool compute;\n' + PROXY)
        proxy = proxy.replace('WRAP(MapMemory,map,mapped);', WRAPS + '\n WRAP(MapMemory,map,mapped);')
        (d / 'proxy.cpp').write_text(proxy)
        flags = ['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I' + str(a.headers)]
        subprocess.run(flags + ['-shared', '-fPIC', str(d / 'proxy.cpp'), '-ldl', '-o', str(d / 'proxy.so')], check=True)
        subprocess.run(flags + ['-fsanitize=address,undefined', '-DFUXI_MTL_HOST_TEST',
                       '-DFUXI_MTL_ICD="' + str(d / 'proxy.so') + '"',
                       '-DFUXI_MTL_TRANSLATOR="' + str(build / 'target/debug/libmetal2vulkan.so') + '"',
                       '-I' + str(ROOT / 'tools'), str(ROOT / 'tools/test-fuxi-metal-m6b.cpp'),
                       '-ldl', '-pthread', '-o', str(d / 'test')], check=True)
        env = dict(os.environ, SOFTWARE_ICD=str(a.icd.resolve()), ASAN_OPTIONS='detect_leaks=0')
        p = subprocess.run([str(d / 'test'), str(a.metallib.resolve())], env=env, text=True, capture_output=True, timeout=30)
        print(p.stdout, end='');print(p.stderr, end='')
        assert p.returncode == 0, 'CI AIR sampling failed'
        assert p.stdout.count('PASS: Vulkan descriptor') == 4, 'per-draw Vulkan contract missing'
        assert 'translation cache HIT' in p.stderr, 'cache path missing'
        assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr


if __name__ == '__main__':
    main()
