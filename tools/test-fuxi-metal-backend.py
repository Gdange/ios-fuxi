#!/usr/bin/env python3
"""Exercise the actual M4 backend on an external software ICD, ASan/UBSan."""
import argparse
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
TEST=r'''
#include "fuxi-metal/Backend.hpp"
#include <cassert>
#include <cstdio>
static void verify(fuxi::Buffer *b,size_t begin,size_t n,uint8_t value) {
 for(size_t i=begin;i<begin+n;i++)if(((uint8_t *)b->contents)[i]!=value)throw std::runtime_error("bad byte "+std::to_string(i));
}
int main() {
 try {
  for(unsigned reopen=0;reopen<2;reopen++) {
   fuxi::Backend backend;
   try {backend.allocate(0);assert(false);}catch(const std::runtime_error&) {}
   try {backend.allocate(fuxi::Backend::maxBufferLength+1);assert(false);}catch(const std::runtime_error&) {}
   auto *src=backend.allocate(65536),*dst=backend.allocate(65536);
   for(unsigned i=0;i<16;i++) {
    uint8_t value=uint8_t(0xa0+i);memset(src->contents,0x39,src->length);memset(dst->contents,0x52,dst->length);
    backend.submit({{nullptr,src,0,4,65528,value},{src,dst,4,8,65520,0}});
    verify(src,0,4,0x39);verify(src,4,65528,value);verify(src,65532,4,0x39);
    verify(dst,0,8,0x52);verify(dst,8,65520,value);verify(dst,65528,8,0x52);
   }
   backend.destroy(src);backend.destroy(dst);
  }
  puts("PASS: actual backend 32 GPU fill/copy submissions, CPU cache transitions, sentinel edges, reuse, reopen");
 } catch(const std::exception &e) {fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
'''

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--headers',type=Path,required=True)
    ap.add_argument('--icd',type=Path,default=Path('/lib/x86_64-linux-gnu/libvulkan_lvp.so'));a=ap.parse_args()
    spec=importlib.util.spec_from_file_location('m3',ROOT/'tools/test-fuxi-vk-smoke.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);proxy=m.PROXY.replace('getenv("TU_DEBUG"),"nocb"','getenv("TU_DEBUG"),"sysmem,nocb"').replace('static bool compute;','static bool compute,corrupted;').replace(
            'if(getenv("BAD_FILL") || (compute && getenv("BAD_COMPUTE")))static_cast<uint32_t *>(pointer)[16383]^=1;',
            '').replace('return wait(d,n,f,all,ns);',
            'VkResult result=wait(d,n,f,all,ns);if(result==VK_SUCCESS && !corrupted && getenv("BAD_FILL")){static_cast<uint32_t *>(pointer)[16383]^=1;corrupted=true;}return result;')
        (d/'proxy.cpp').write_text(proxy);(d/'test.cpp').write_text(TEST)
        flags=['clang++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(a.headers)]
        subprocess.run(flags+['-shared','-fPIC',str(d/'proxy.cpp'),'-ldl','-o',str(d/'proxy.so')],check=True)
        subprocess.run(flags+['-fsanitize=address,undefined','-DFUXI_MTL_HOST_TEST','-DFUXI_MTL_ICD="'+str(d/'proxy.so')+'"',
            '-I'+str(ROOT/'tools'),str(d/'test.cpp'),'-ldl','-pthread','-o',str(d/'test')],check=True)
        env=dict(os.environ,SOFTWARE_ICD=str(a.icd),ASAN_OPTIONS='detect_leaks=0')
        for extra,expected in (({},0),({'TIMEOUT':'1'},1),({'LOST':'1'},1),({'BAD_FILL':'1'},1)):
            p=subprocess.run([str(d/'test')],env=dict(env,**extra),text=True,capture_output=True,timeout=30)
            assert p.returncode==expected,(extra,p.stdout,p.stderr)
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr,p.stderr
            if not extra:print(p.stdout,end='')
            else:assert 'FAIL' in p.stderr,p.stderr
        print('PASS: injected fence timeout, device loss, last-byte corruption stop execution')
if __name__=='__main__':main()
