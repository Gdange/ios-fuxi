#!/usr/bin/env python3
"""Linux runtime-model tests; native Mac frontend/smoke tests with CPU model.

Synthetic 528-byte base/root/registry ID and CPU transfers; no iOS discovery or
GPU execution is qualified by these tests. Production backend tested separately.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--compile-only',action='store_true',help='build all applicable tests without running executables')
    a=ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        d=Path(tmp);conf=d/'smoke.conf'
        compiler=['xcrun','clang++'] if os.uname().sysname=='Darwin' else ['clang++']
        subprocess.run([*compiler,'-std=c++17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-pthread',
                        str(ROOT/'tools/fuxi-metal/ConstantHooksTests.cpp'),'-o',str(d/'constant-hooks')],check=True)
        if not a.compile_only:subprocess.run([str(d/'constant-hooks')],check=True,timeout=15)
        subprocess.run([*compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined',str(ROOT/'tools/fuxi-metal/VertexProfileTests.cpp'),
                        '-o',str(d/'vertex-profile')],check=True)
        if not a.compile_only:subprocess.run([str(d/'vertex-profile')],check=True,timeout=15)
        subprocess.run([*compiler,'-std=c++17','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined',str(ROOT/'tools/fuxi-metal/TimelineTests.cpp'),
                        '-o',str(d/'timeline')],check=True)
        if not a.compile_only:subprocess.run([str(d/'timeline')],check=True,timeout=15)
        subprocess.run([*compiler,'-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined',str(ROOT/'tools/fuxi-metal/ResourceCacheTests.cpp'),
                        '-o',str(d/'resource-cache')],check=True)
        if not a.compile_only:subprocess.run([str(d/'resource-cache')],check=True,timeout=30)
        if os.uname().sysname!='Darwin':
            print('SKIP: native Objective-C frontend/smoke tests require Mac Foundation/Metal')
            return
        flags=['-O1','-g','-Wall','-Wextra','-Werror','-fno-objc-arc','-fblocks','-fsanitize=address,undefined']
        frameworks=['-framework','Foundation','-framework','Metal','-framework','IOSurface']
        def call(command):subprocess.run(command,check=True)
        if a.compile_only:
            for name in ['ConstantFrontendTests','FrontendTests','PipelineFrontendTests','CAFrontendTests','GateFrontendTests','CensusTests']:
                call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal'/(name+'.mm')),*frameworks,'-o',str(d/name)])
            print('PASS: compile-only frontend tests; no test executables run')
            return
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/CAFrontendTests.mm'),*frameworks,'-o',str(d/'ca-frontend')])
        subprocess.run([str(d/'ca-frontend')],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),check=True,timeout=15)
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/ConstantFrontendTests.mm'),*frameworks,'-o',str(d/'constant-frontend')])
        constants=subprocess.run([str(d/'constant-frontend')],env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'),text=True,capture_output=True,check=True,timeout=15)
        failures=[line for line in constants.stderr.splitlines() if 'constants snapshot FAIL' in line]
        assert len(failures)==8,constants.stderr
        assert all(len(line)<4096 and 'chain=' in line and 'ctor_us=' in line and 'gate_us=' in line and 'install_us=' in line
                   and 'record=' in line and 'setConstantValue:type:atIndex:=' in line and 'imp=actual/hook/original' in line for line in failures),constants.stderr
        assert 'AddressSanitizer' not in constants.stderr and 'runtime error:' not in constants.stderr,constants.stderr
        print(constants.stdout,end='')
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/FrontendTests.mm'),*frameworks,'-o',str(d/'frontend')])
        env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/PipelineFrontendTests.mm'),*frameworks,'-o',str(d/'pipeline-frontend')])
        subprocess.run([str(d/'pipeline-frontend')],env=env,check=True,timeout=15)
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/GateFrontendTests.mm'),*frameworks,'-o',str(d/'gate')])
        gate=subprocess.run([str(d/'gate'),'daemon-argv'],env=env,text=True,capture_output=True,check=True,timeout=10)
        assert gate.stderr.count('FUXI MTL: gate deny gate-test')==1,gate.stderr
        print(gate.stdout,end='')
        allowed=subprocess.run([str(d/'gate'),'daemon-argv'],env=dict(env,FUXI_TEST_GATE_ALLOW='1'),text=True,capture_output=True,check=True,timeout=10)
        assert allowed.stderr.count('FUXI MTL: gate allow gate-test')==1,allowed.stderr
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/ClientGateTests.cpp'),'-o',str(d/'policy')])
        subprocess.run([str(d/'policy')],env=env,check=True,timeout=10)
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/CensusTests.mm'),*frameworks,'-o',str(d/'census')])
        census=subprocess.run([str(d/'census')],env=env,text=True,capture_output=True,check=True,timeout=15)
        assert census.stderr.count(' supportsFamily: implemented')==1,census.stderr
        assert census.stderr.count('FUXI MTL: census census-test ')==256,census.stderr
        assert census.stderr.count('FUXI MTL: census limit census-test 256')==1,census.stderr
        assert 'AddressSanitizer' not in census.stderr and 'runtime error:' not in census.stderr,census.stderr
        print(census.stdout,end='')
        subprocess.run([str(d/'frontend')],env=env,check=True,timeout=15)
        call(['xcrun','clang',*flags,'-DMTLCreateSystemDefaultDevice=FuxiTestCreateDevice','-Dobjc_getClass=FuxiTestGetClass',
            '-Dgeteuid=FuxiTestGeteuid','-Dalarm=FuxiTestAlarm','-DFUXI_MTL_CONF="'+str(conf)+'"',
            '-c',str(ROOT/'tools/fuxi-metal-smoke.m'),'-o',str(d/'smoke.o')])
        call(['xcrun','clang++','-std=c++17',*flags,str(ROOT/'tools/fuxi-metal/SmokeTestDevice.mm'),str(d/'smoke.o'),*frameworks,'-o',str(d/'smoke')])
        def run(data,rc=0,**extra):
            if data is None:conf.unlink(missing_ok=True)
            else:conf.write_bytes(data)
            p=subprocess.run([str(d/'smoke'),'--host-daemon-argv','ignored'],env=dict(env,**extra),text=True,capture_output=True,timeout=10)
            assert p.returncode==rc,(p.returncode,p.stdout,p.stderr)
            assert 'AddressSanitizer' not in p.stderr and 'runtime error:' not in p.stderr,p.stderr
            return p.stdout
        print(run(None),end='')
        for stage in range(4):
            out=run(str(stage).encode());assert 'stage %u '%stage in out and 'done PASS' in out
            assert 'stage %u '%(stage+1) not in out
        for bad in (b'',b'7',b'-1',b'1junk',b'3\0',b'1'*32):assert 'stage 0 discovery FAIL' in run(bad,1)
        assert 'stage 4 pipeline FAIL' in run(b'4',1)
        assert 'stage 0 discovery FAIL' in run(b'3',1,FUXI_TEST_NOT_ROOT='1')
        assert 'returned nil' in run(b'3',1,FUXI_TEST_NO_DEVICE='1')
        assert 'stage 1 fill FAIL' in run(b'3',1,FUXI_TEST_LOST='1')
        assert 'byte 65535' in run(b'3',1,FUXI_TEST_BAD_DATA='1')
        assert 'stage 1 fill FAIL exceeded 30 s' in run(b'3',124,FUXI_TEST_HANG='1')
        print('PASS: actual smoke stages/default/config/ignored argv, nil discovery, nonroot, submit loss, last-byte corruption, alarm exit')
if __name__=='__main__':main()
