#!/usr/bin/env python3
"""Actual external AIR translator + software Vulkan integration, sanitizer/fault tests.
Requires our Mac-built metallib, external host translator and Vulkan headers/ICD.
"""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--headers',type=Path,required=True)
    ap.add_argument('--translator',type=Path,required=True)
    ap.add_argument('--metallib',type=Path,required=True)
    ap.add_argument('--icd',type=Path,default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'))
    a=ap.parse_args()
    spec=importlib.util.spec_from_file_location('m3',ROOT/'tools/test-fuxi-vk-smoke.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp)
        proxy=m.PROXY.replace('#include <unistd.h>','#include <unistd.h>\n#include <cstdio>')
        proxy=proxy.replace('static bool compute;', '''static bool compute;
static PFN_vkCreateShaderModule createShader;
static unsigned shaderIndex;
static VkResult shader(VkDevice d,const VkShaderModuleCreateInfo *ci,const VkAllocationCallbacks *alloc,VkShaderModule *out) {
 char path[4096];snprintf(path,sizeof(path),"%s/%u.spv",getenv("SPV_DIR"),shaderIndex++);
 FILE *f=fopen(path,"wb");assert(f);assert(fwrite(ci->pCode,1,ci->codeSize,f)==ci->codeSize);fclose(f);
 return createShader(d,ci,alloc,out);
}''')
        proxy=proxy.replace('WRAP(MapMemory,map,mapped);','WRAP(CreateShaderModule,createShader,shader);WRAP(MapMemory,map,mapped);')
        proxy=proxy.replace('static_cast<uint32_t *>(pointer)[16383]^=1','static_cast<uint32_t *>(pointer)[65599]^=1')
        (d/'proxy.cpp').write_text(proxy)
        flags=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(a.headers)]
        subprocess.run(flags+['-shared','-fPIC',str(d/'proxy.cpp'),'-ldl','-o',str(d/'proxy.so')],check=True)
        subprocess.run(flags+['-fsanitize=address,undefined','-DFUXI_MTL_HOST_TEST',
            '-DFUXI_MTL_ICD="'+str(d/'proxy.so')+'"','-DFUXI_MTL_TRANSLATOR="'+str(a.translator.resolve())+'"',
            '-I'+str(ROOT/'tools'),str(ROOT/'tools/test-fuxi-metal-pipeline.cpp'),'-ldl','-pthread','-o',str(d/'test')],check=True)
        env=dict(os.environ,SOFTWARE_ICD=str(a.icd),SPV_DIR=str(d),ASAN_OPTIONS='detect_leaks=0')
        for extra,rc in (({},0),({'TIMEOUT':'1'},1),({'LOST':'1'},1),({'BAD_FILL':'1'},1)):
            p=subprocess.run([str(d/'test'),str(a.metallib)],env=dict(env,**extra),text=True,capture_output=True,timeout=30)
            assert p.returncode==rc,(extra,p.stdout,p.stderr)
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr,p.stderr
            if not extra:
                assert p.stdout.count('PASS: translated pipeline')==4,p.stdout
                print(p.stdout,end='');print(p.stderr,end='')
                for spv in sorted(d.glob('*.spv')):
                    subprocess.run(['spirv-val','--target-env','vulkan1.2',str(spv)],check=True)
            else:assert 'FAIL:' in p.stderr,p.stderr
    print('PASS: real translated SPIR-V Vulkan 1.2 validation; corrupt library/name refusal; loss/timeout/last-pixel corruption fail closed')


if __name__=='__main__':main()
