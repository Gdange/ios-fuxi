#!/usr/bin/env python3
"""Offline execswap parser, actual trace/quota methods and stage migration tests."""
from pathlib import Path
import importlib.util
import os
import re
import struct
import subprocess
import tempfile
import plistlib
ROOT=Path(__file__).resolve().parents[1]

def module(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'tools'/f'{name}.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

def main():
    subprocess.run(['python3',str(ROOT/'tools/sync-fuxi-metal2.py'),'--check'],check=True)
    with tempfile.TemporaryDirectory() as tmp:
        temp=Path(tmp)
        parser=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "sweet_execswap.h"
static bool parse(const char *arg) {
 struct sweet_execswap s={0};strncpy(s.arg,arg,sizeof(s.arg)-1);
 return sweet_execswap_parse(&s);
}
int main(void) {
 struct sweet_execswap s={0};
 assert(!sweet_execswap_match(&s,"/usr/sbin/bluetoothd"));
 strcpy(s.arg,"bluetoothd:/usr/local/bin/fuxi-gpu-abi-test");assert(sweet_execswap_parse(&s));
 assert(!strcmp(sweet_execswap_match(&s,"/usr/sbin/bluetoothd"),"/usr/local/bin/fuxi-gpu-abi-test"));
 assert(sweet_execswap_match(&s,"bluetoothd"));
 assert(!sweet_execswap_match(&s,"/usr/sbin/bluetoothd-extra"));
 assert(!sweet_execswap_match(&s,"/bluetoothd/other"));
 assert(!sweet_execswap_match(&s,"/bluetoothd/"));
 const char *bad[]={"",":/test","bluetoothd:","bluetoothd:relative","bluetoothd:/",
 "a,b:/test","a:/test,b:/other","a:/test:other","/a:/test","a :/test","a:/test\tbad",
 "12345678901234567890123456789012:/test"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++) assert(!parse(bad[i]));
 char big[193];memset(big,'a',sizeof(big));big[1]=':';big[2]='/';big[192]=0;
 assert(!parse(big));
 assert(parse("1234567890123456789012345678901:/test"));
 puts("PASS: default-off/exact basename, one pair, absolute target, bounds/truncation and malformed execswap");
}'''
        p=temp/'parser.c';p.write_text(parser)
        subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'xnu/fuxi-exec'),str(p),'-o',str(temp/'parser')],check=True)
        subprocess.run([str(temp/'parser')],check=True)
        patch=(ROOT/'xnu/patches/0100-sweet-execswap-gpu-abi-trace.patch').read_text()
        added='\n'.join(line[1:] for line in patch.splitlines() if line.startswith('+') and not line.startswith('+++'))
        glue=added[added.index('#include "sweet_execswap.h"'):added.index('\n#if defined(ARM64_BOARD_CONFIG_SWEET)')]
        # The closing brace was a context line in the unified diff.
        glue+='\n}\n'
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#define LCK_MTX_DECLARE(name,group) int name
static void lck_mtx_lock(int *l) {(void)l;}
static void lck_mtx_unlock(int *l) {(void)l;}
static const char *bootarg;static unsigned parses,logs;static char last[256];
static bool PE_parse_boot_argn(const char *key,char *out,size_t n) {
 assert(!strcmp(key,"sweetexecswap"));++parses;
 if (!bootarg)return false;
 snprintf(out,n,"%s",bootarg);return true;
}
static bool sweet_exec_blocked(const char *name,bool log) {assert(!log);return !strcmp(name,"bluetoothd");}
static int trace(const char *fmt,...) {va_list a;va_start(a,fmt);vsnprintf(last,sizeof(last),fmt,a);va_end(a);++logs;return 0;}
#define printf trace
'''+glue+r'''
#undef printf
int main(int argc,char **argv) {
 bootarg=argc==2 ? argv[1] : NULL;
 assert(!sweet_exec_swap("/sbin/other",123));
 for(unsigned i=0;i<12;i++) {
  const char *path=sweet_exec_swap("/usr/sbin/bluetoothd",123);
  assert((path!=NULL)==(bootarg!=NULL));
  if(path)assert(!strcmp(path,"/usr/local/bin/fuxi-gpu-abi-test"));
 }
 assert(parses==1 && logs==(bootarg?8u:0u));
 if(bootarg)assert(!strcmp(last,"SWEET EXECSWAP: bluetoothd -> /usr/local/bin/fuxi-gpu-abi-test pid 123 (wins over sweetnoexec)\n"));
 puts("PASS: actual exec glue parse once, default off, eight-line cap and sweetnoexec precedence log");
}
'''
        p=temp/'exec.c';p.write_text(harness)
        subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'xnu/fuxi-exec'),str(p),'-o',str(temp/'exec')],check=True)
        subprocess.run([str(temp/'exec')],check=True)
        subprocess.run([str(temp/'exec'),'bluetoothd:/usr/local/bin/fuxi-gpu-abi-test'],check=True)
        # Compile the actual result/mount/trace code with memory and lock stubs.
        client=(ROOT/'xnu/fuxi-adreno/FuxiAdrenoUserClient.cpp').read_text()
        trace=client[client.index('static bool connected;'):client.index('class FuxiAdrenoUserClient :')]
        result=client[client.index(' void result() {'):client.index(' uint64_t nextHandle_')]
        mount=client[client.index(' bool mount() {'):client.index(' IOReturn externalMethod(')]
        model=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "sweet_execswap.h"
using IOReturn=unsigned;
static constexpr unsigned kIOReturnSuccess=0,kIOReturnIOError=0xe00002bc,kIOReturnBusy=0xe00002d5;
static int openLock;
static void IOLockLock(int) {} static void IOLockUnlock(int) {}
static unsigned logs;static char last[256];
static void IOLog(const char *fmt,...) {va_list ap;va_start(ap,fmt);vsnprintf(last,sizeof(last),fmt,ap);va_end(ap);++logs;}
static bool fuxi_gpu_abi_available() {return true;}
struct Memory { uint32_t words[16384]={};void *getBytesNoCopy() {return words;} };
'''+trace+r'''
struct Client {
 struct {Memory *memory=nullptr;uint64_t bytes=65536,iova=0x20000000;} bos_[1];
 bool live_=false,diagnostic_=true,resultChecked_=false;int pid_=123;
'''+result+mount+r'''
};
int main() {
 assert(!execswapTest && !testPasses);
 assert(abiExecswapTest("IOMFB_FDR_Loader:/usr/local/bin/fuxi-gpu-abi-test"));
 assert(abiExecswapTest("bluetoothd:/usr/local/bin/fuxi-gpu-abi-test"));
 assert(!abiExecswapTest("IOMFB_FDR_Loader:/usr/local/bin/other"));
 assert(!abiExecswapTest("IOMFB_FDR_Loader:/usr/local/bin/fuxi-gpu-abi-test:other"));
 assert(!abiExecswapTest("IOMFB_FDR_Loader:/usr/local/bin/fuxi-gpu-abi-test,other:/test"));
 assert(!abiExecswapTest(""));
 // Compare actual diagnostic recognition against the exec parser's name rules.
 for (unsigned len=0;len<=32;++len) for (unsigned byte=1;byte<=255;++byte) {
  char arg[192]={};memset(arg,'a',len);
  if (len)arg[len/2]=(char)byte;
  strcat(arg,":/usr/local/bin/fuxi-gpu-abi-test");
  sweet_execswap s={};strcpy(s.arg,arg);
  bool parsed=sweet_execswap_parse(&s);
  assert(abiExecswapTest(arg)==(parsed && !strcmp(s.path,"/usr/local/bin/fuxi-gpu-abi-test")));
  if(parsed)assert(sweet_execswap_match(&s,s.name)==s.path);
 }
 Memory mem;Client a;a.bos_[0].memory=&mem;
 assert(a.mount());a.result();assert(!testPasses && strstr(last,"bad-pixels 256"));
 connected=false;
 mem.words[0x1000/4]=0xa740cafe;
 for(unsigned i=0;i<256;i++)mem.words[0x2000/4+i]=0xff336699;
 Client b;b.bos_[0].memory=&mem;assert(b.mount());b.result();b.result();assert(testPasses==1);
 connected=false;
 Client c;c.bos_[0].memory=&mem;assert(c.mount());c.result();assert(testPasses==2);
 connected=false;
 Client d;assert(!d.mount() && strstr(last,"already passed 2/2 rounds this boot"));
 d.diagnostic_=false;assert(d.mount());connected=false;
 Client e;e.diagnostic_=false;e.bos_[0].memory=&mem;e.result();assert(testPasses==2);
 assert(!strcmp(abiStep(1),"query") && !strcmp(abiStep(6),"wait"));
 for(unsigned i=0;i<100;i++)abiTrace("open",123,0);
 assert(traceLines==64 && logs==64);
 puts("PASS: actual result readback, failed pixels, two verified rounds, repeat refusal, unrelated client, 64-line trace cap");
}'''
        model=re.sub(r'__asm__ volatile\("dsb sy" ::: "memory"\);','(void)0;',model)
        p=temp/'guard.cpp';p.write_text(model)
        subprocess.run(['clang++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'xnu/fuxi-exec'),str(p),'-o',str(temp/'guard')],check=True)
        subprocess.run([str(temp/'guard')],check=True)
        # Run the real lane in an isolated tree, stubbing only the packers.
        pack=temp/'pack';(pack/'tools').mkdir(parents=True)
        (pack/'tools/build-fuxi-metal2-images.sh').write_bytes((ROOT/'tools/build-fuxi-metal2-images.sh').read_bytes())
        base=pack/'base';base.mkdir()
        basecmd='fuxigpu=1 fuxigpuabi=1 sweetnoexec=fairplayd.H2 sweetexecswap=bluetoothd:/usr/local/bin/fuxi-gpu-abi-test'
        (base/'cmdline.txt').write_text(basecmd+'\n');(base/'fuxi.adt').write_bytes(b'adt')
        out=pack/'xnu/out-fuxi-metal2e';out.mkdir(parents=True)
        for name in ('kernel.development.fuxi','kernel.development.fuxi.unstripped'):
            (out/name).write_bytes(b'kernel')
        (pack/'tools/out').mkdir();(pack/'tools/out/fuxi-gpu-abi-test').write_bytes(b'test')
        (pack/'tools/mkpayload.py').write_text('import pathlib,sys\npathlib.Path(sys.argv[3]).write_bytes(b"payload")\n')
        (pack/'shim/fuxi-loader').mkdir(parents=True)
        required=('FUXI_GMU_FIRMWARE','FUXI_SQE_FIRMWARE','FUXI_GPU_ZAP_MBN','MKBOOTIMG')
        (pack/'shim/fuxi-loader/build.sh').write_text('''#!/bin/bash
set -eu
python3 - <<'PY'
import os
from pathlib import Path
for key in ('FUXI_GMU_FIRMWARE','FUXI_SQE_FIRMWARE','FUXI_GPU_ZAP_MBN','MKBOOTIMG'):
    assert Path(os.environ[key]).is_absolute() and Path(os.environ[key]).is_file()
out=Path('shim/out/fuxi-loader');out.mkdir(parents=True)
for name in ('fuxi-xnu-boot.img','loader.elf'): (out/name).write_bytes(b'packed')
PY
''')
        env=dict(os.environ)
        for key in required:env.pop(key,None)
        command=['bash','tools/build-fuxi-metal2-images.sh',str(base),'metal2e']
        for key in required:
            source=pack/(key+' input');source.write_bytes(b'input');env[key]=str(source)
        for key in required:
            for value in (None,'relative/path'):
                badenv=dict(env)
                if value is None:badenv.pop(key)
                else:badenv[key]=value
                run=subprocess.run(command,cwd=pack,env=badenv,capture_output=True,text=True)
                assert run.returncode and key in run.stderr,run
        subprocess.run(command,cwd=pack,env=env,check=True)
        expected=basecmd.replace('sweetnoexec=fairplayd.H2','sweetnoexec=bluetoothd,fairplayd.H2').replace('sweetexecswap=bluetoothd:','sweetexecswap=IOMFB_FDR_Loader:')
        assert (out/'cmdline.txt').read_text()==expected+'\n'
        for invalid in (basecmd+' fuxiwlan=1',basecmd+' sweetexecswap=other:/test',basecmd+' sweetnoexec=other',basecmd+' '+('x'*608)):
            (base/'cmdline.txt').write_text(invalid+'\n')
            assert subprocess.run(command,cwd=pack,env=env,capture_output=True).returncode
        print('PASS: metal2e exact cmdline, malformed/oversize base refusal, mandatory absolute firmware/tool paths forwarded to loader')
        stage=module('stage-fuxi-gpu-abi-test')
        root=temp/'root';cache=root/'System/Library/xpc/launchd.plist';cache.parent.mkdir(parents=True)
        other={'Label':'com.example.other','RunAtLoad':True}
        cache.write_bytes(plistlib.dumps({'LaunchDaemons':{stage.LEGACY_JOB:stage.job(),'/other':other}},fmt=plistlib.FMT_BINARY))
        legacy=root/stage.LEGACY_JOB.lstrip('/');legacy.parent.mkdir(parents=True);legacy.write_text('old')
        binary=temp/'test';binary.write_bytes(struct.pack('<3I',0xfeedfacf,0x100000c,0))
        stage.stage(root,binary)
        jobs=plistlib.loads(cache.read_bytes())['LaunchDaemons']
        assert jobs=={'/other':other,stage.JOB:stage.job()} and not legacy.exists()
        assert stage.JOB.startswith('/System/Library/LaunchDaemons/')
        assert stage.job()['Program']==stage.BINARY
        assert (root/stage.BINARY.lstrip('/')).stat().st_mode&0o777==0o755
        stage.stage(root,binary,watchdog=True)
        assert plistlib.loads(cache.read_bytes())['LaunchDaemons'][stage.JOB]['ProgramArguments'][-1]=='--watchdog'
        stage.stage(root,remove=True)
        assert plistlib.loads(cache.read_bytes())['LaunchDaemons']=={'/other':other}
        assert not (root/stage.JOB.lstrip('/')).exists()
        print('PASS: stage system bootstrap path, legacy migration, idempotence, watchdog/removal, other jobs preserved')
    patch=(ROOT/'xnu/patches/0100-sweet-execswap-gpu-abi-trace.patch').read_text()
    assert '!(imgp->ip_flags & IMGPF_SWEET_EXECSWAP)' in patch
    assert 'sweet_exec_blocked(swap.name, false)' in patch and 'wins over sweetnoexec' in patch
    assert 'logged < 8' in patch and 'if (!parsed)' in patch
    assert 'clientHasPrivilege(securityID,kIOClientPrivilegeAdministrator)!=kIOReturnSuccess) return kIOReturnNotPrivileged;' in client
    assert 'abiExecswapTest(swap)' in client
    print('PASS: execswap precedence marker, bounded logging and serialized parse-once wiring')
if __name__=='__main__':main()
