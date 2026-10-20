#!/usr/bin/env python3
"""Offline native-memory model of gpu.c, never opens a device.

Removes only AArch64 barriers, substitutes the counter and hooks MMIO writes.
This checks C protocol/memory logic, not SMMU hardware or firmware execution.
Optional external firmware input exercises the exact local block image too.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREFIX = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
static uintptr_t probe_fdt;
static uint32_t probe_fdt_size;
char _head[1], _end[1];
extern uint8_t _gpu_fw_start[];
static uint64_t ticks;
#define rsr(reg) (strcmp(reg,"cntfrq_el0") ? ++ticks : 1000000ull)
static void delay(unsigned us) { ticks += us; }
static void mock_write(uint64_t at, uint32_t val);
'''
SUFFIX = r'''
static int firmware_success = 1, ack_mode, corrupt_readback, tlb_busy;
static unsigned firmware_releases, mmio_writes;
static uint32_t *ptr(uint64_t at) { return (uint32_t *)(uintptr_t)at; }
static void mock_write(uint64_t at, uint32_t val)
{
 mmio_writes++;
 if (at >= GPU_SMMU_BASE + g_cbbase && at < GPU_SMMU_BASE + 0x40000) {
  unsigned off = (at-GPU_SMMU_BASE-g_cbbase)%g_stride;
  if (off == 0x58) *ptr(at) = 0; /* W1C */
  if (off == 0x7f0) *ptr(at+4) = tlb_busy;
  if (off == 0x30 && corrupt_readback) *ptr(at) ^= 1;
 }
 if (at == GPU_GMU_CORE + 0x5000*4 && !val) {
  firmware_releases++;
  if (firmware_success) *ptr(GPU_GMU_CORE+0x501c*4) = 0x100;
 }
 if (at == GPU_GMU_CORE + 0x5186*4 && val) *ptr(GPU_GMU_CORE+0x5180*4) = 1;
 if (at == GPU_GMU_CORE + 0x5191*4) *ptr(GPU_GMU_CORE+0x5192*4) &= ~val;
 if (at != GPU_GMU_CORE + 0x5194*4) return;
 if (val == (1u<<30)) {
  *ptr(GPU_GMU_CORE+0x5192*4) |= 1u<<31;
  *ptr(GPU_GPUCC_BASE + CC_GX_GDSCR) = 1u<<31;
 }
 if (val != 1 || ack_mode == 1) return;
 unsigned rd = *ptr(G_HFI_PA+64), wr = *ptr(G_HFI_PA+68);
 unsigned hdr = *ptr(G_HFI_PA+4096+rd*4);
 unsigned q = *ptr(G_HFI_PA+116);
 *ptr(G_HFI_PA+8192+q*4) = (hdr&0xfff00000) | (1u<<16) | (3u<<8) | 126;
 *ptr(G_HFI_PA+8192+((q+1)&1023)*4) = hdr ^ (ack_mode == 2 ? 1u<<20 : 0);
 *ptr(G_HFI_PA+8192+((q+2)&1023)*4) = ack_mode == 3 ? 9 : 0;
 if (ack_mode == 4) *ptr(G_HFI_PA+8192+q*4) = 126; /* impossible size0 */
 *ptr(G_HFI_PA+116) = (q+4)&1023;
 *ptr(G_HFI_PA+64) = wr;
 *ptr(GPU_GMU_CORE+0x5192*4) |= 1;
}
static void reset_model(void)
{
 memset((void *)(uintptr_t)GPU_REG_BASE, 0, 0x100000);
 memset((void *)(uintptr_t)G_MEM, 0, G_MEM_SIZE);
 memset(gpu_report, 0, sizeof(gpu_report)); memset(gpu_extra, 0, sizeof(gpu_extra));
 gpu_snapshot_valid = gpu_gmu_started = gpu_m2_attempted = gpu_s8_ok = 0;
 gpu_s9_attempted = gpu_s9_ok = gpu_s10_attempted = 0;
 ticks = firmware_releases = mmio_writes = 0;
 ack_mode = corrupt_readback = tlb_busy = 0; firmware_success = 1;
 *ptr(GPU_SMMU_BASE+0x20) = 0x4c017e18;
 *ptr(GPU_SMMU_BASE+0x24) = 0x40000016;
 *ptr(GPU_SMMU_BASE+0x28) = 0x5511;
 *ptr(GPU_SMMU_BASE) = 0x2f0406;
 *ptr(GPU_GMU_CORE+0x5000*4) = 1;
 *ptr(GPU_GMU_CORE+(0x1c00+0xff8)*4) = 0x20010005;
 gpu_extra[0][0] = 0xa7400002;
 assert(g_snapshot());
}
static void aux_fixture(void)
{
 memset(gpu_cmddb, 0, sizeof(gpu_cmddb));
 uint16_t gfx[] = {0,0x34,0x38,0x3c,0x40,0x50,0x80,0x90,0xc0,0xe0};
 uint16_t mx[] = {0,0x40,0x80,0xe0}, cx[] = {0,0x40,0x80};
 const char names[3][8] = {"gfx.lvl","mx.lvl","cx.lvl"};
 uint8_t *h = gpu_cmddb+8; h[0] = 3; h[4] = 0; h[5] = 2; h[6] = 3;
 unsigned off = 0;
 for (unsigned i=0;i<3;i++) {
  uint8_t *p = gpu_cmddb+144+i*24;
  memcpy(p,names[i],8);
  unsigned len = i == 0 ? sizeof(gfx) : i == 1 ? sizeof(mx) : sizeof(cx);
  p[20] = len; p[22] = off;
  memcpy(gpu_cmddb+144+512+off, i == 0 ? gfx : i == 1 ? mx : cx, len); off += len;
 }
 for (unsigned i=0;i<4;i++) gpu_report[87+i] = 0x50000+i*4;
}
static uint64_t walk(unsigned va)
{
 uint64_t *l1=(uint64_t *)(uintptr_t)G_MEM;
 if (!l1[va>>30]) return 0;
 uint64_t *l2=(uint64_t *)(uintptr_t)(l1[va>>30]&~0xfffull);
 if (!l2[(va>>21)&511]) return 0;
 uint64_t *l3=(uint64_t *)(uintptr_t)(l2[(va>>21)&511]&~0xfffull);
 return l3[(va>>12)&511];
}
int main(int argc, char **argv)
{
 (void)argc; (void)argv;
 assert(mmap((void *)(uintptr_t)GPU_REG_BASE, 0x100000, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 assert(mmap((void *)(uintptr_t)0x0b280000, 0x10000, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 assert(mmap((void *)(uintptr_t)G_MEM, G_MEM_SIZE, PROT_READ|PROT_WRITE,
  MAP_ANONYMOUS|MAP_PRIVATE|MAP_FIXED_NOREPLACE,-1,0) != MAP_FAILED);
 reset_model(); unsigned before=mmio_writes;
 assert(g_snapshot() && before == mmio_writes && g_ncb == 22 && g_nsmr == 24 && g_cbbase == 0x20000);
 /* An active, referenced CB1 must be preserved; select CB2 instead. */
 *ptr(GPU_SMMU_BASE+SMMU_SMR(0))=0x80000000;
 *ptr(GPU_SMMU_BASE+SMMU_S2CR(0))=1;
 *ptr(GPU_SMMU_BASE+g_cb(1))=1;
 *ptr(GPU_SMMU_BASE+g_cb(1)+0x24)=1u<<16;
 assert(g_snapshot() && g_select_context() && g_bank==2 && g_slot==1 && g_asid==2);
 *ptr(GPU_SMMU_BASE+SMMU_SMR(1))=0x80000005;
 assert(g_snapshot() && !g_select_context() && gpu_extra[0][2]==0x804);
 /* Masked existing SID match must also refuse. */
 *ptr(GPU_SMMU_BASE+SMMU_SMR(1))=0x80070000;
 assert(g_snapshot() && !g_select_context());
 reset_model(); aux_fixture();
 assert(g_firmware(0));
 /* Corrupt external format headers in anonymous process memory, then restore. */
 uint32_t *header=(uint32_t *)(uintptr_t)_gpu_fw_start;
 uint32_t save_size=header[1], save_addr=header[0];
 header[1]=0xfffffffcu; assert(!g_firmware(0) && gpu_extra[0][2]==0x803); header[1]=save_size;
 header[0]=0xfffffffcu; assert(!g_firmware(0) && gpu_extra[0][2]==0x803); header[0]=save_addr;
 assert(g_firmware(0));
 assert(gpu_extra[0][18] == EXPECT_BLOCKS && gpu_extra[0][19] == EXPECT_PREALLOC);
 gpu_m2(8); assert(gpu_s8_ok && !gpu_gmu_started && g_bank==1);
 assert(gpu_extra[0][33] == 13);
 for(unsigned a=0x4000;a<0x1000000;a+=4096) {
  uint64_t pte=walk(a); assert((pte&~0xfffull)==G_IC_PA+a-0x4000 && !(pte&64));
 }
 assert(!walk(0) && !walk(0x1000000));
 assert((walk(G_LOG_VA)&~0xfffull)==G_LOG_PA && (walk(G_LOG_VA)&64));
 assert((walk(G_HFI_VA+0x3000)&~0xfffull)==G_HFI_PA+0x3000);
 assert((walk(G_DEBUG_VA+0x6000)&~0xfffull)==G_DEBUG_PA+0x6000);
 assert(!walk(G_DEBUG_VA+0x7000) && !walk(G_HFI_VA+0x4000));
 gpu_m2(9); assert(gpu_s9_ok && gpu_gmu_started && firmware_releases==1);
 assert(gpu_extra[0][40]==0x103b04 && gpu_extra[0][44]==0x20a003);
 for(unsigned i=0;i<4;i++) assert(gpu_extra[0][40+i*4]==gpu_extra[0][42+i*4] && gpu_extra[0][43+i*4]==0);
 before=mmio_writes; gpu_probe(9); assert(before==mmio_writes && firmware_releases==1 && gpu_gmu_started);
 gpu_m2(10); assert(gpu_extra[0][60]==1 && gpu_extra[0][65]==1);
 FILE *f=fopen("report.bin","wb"); assert(f); gpu_report[0]=0xa740;
 fwrite(gpu_report,1,512,f); fwrite(gpu_extra,1,5*512,f); fclose(f);
 /* Queue wrap and each failure are bounded and distinguishable. */
 for(int mode=0;mode<=4;mode++) {
  g_hfi_init(); ack_mode=mode;
  *ptr(G_HFI_PA+64)=*ptr(G_HFI_PA+68)=1020;
  *ptr(G_HFI_PA+112)=*ptr(G_HFI_PA+116)=1020;
  uint32_t msg[5]={0}; int err=g_hfi_send(14,msg,5,0);
  assert(err==(mode==0?0:mode==1?0x903:mode==2?0x906:mode==3?0x907:0x904));
 }
 g_hfi_init(); *ptr(G_HFI_PA+64)=4;
 uint32_t full[4]={0}; assert(g_hfi_send(14,full,4,0)==0x904);
 g_hfi_init(); *ptr(G_HFI_PA+68)=1024;
 uint32_t msg[1]={0}; assert(g_hfi_send(10,msg,1,0)==0x904);
 reset_model(); aux_fixture(); corrupt_readback=1; gpu_m2(8);
 assert(!gpu_s8_ok && gpu_extra[0][2]==0x809 && !gpu_gmu_started);
 reset_model(); aux_fixture(); tlb_busy=1; gpu_m2(8);
 assert(!gpu_s8_ok && gpu_extra[0][2]==0x808 && ticks<=1100);
 reset_model(); aux_fixture(); gpu_m2(8); firmware_success=0; gpu_m2(9);
 assert(!gpu_s9_ok && gpu_extra[0][3]==0x901 && gpu_gmu_started);
 before=mmio_writes; gpu_probe(10); gpu_probe(1);
 assert(before==mmio_writes && gpu_gmu_started && firmware_releases==1);
 assert(!g_fw_dest(0x10007ffc,8) && !g_fw_dest(0xfffffffc,8));
 puts("PASS: SMMU snapshot/ownership/readback, all icache PTEs, firmware load, HFI order/ACK/wrap/errors/timeouts, one-shot GMU/handoff latch");
 return 0;
}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--firmware', type=Path, help='external local gmu_gen70200.bin; never copied into git')
    args = ap.parse_args()
    source = (ROOT / 'shim/fuxi-probe/gpu.c').read_text()
    source = source.replace('extern const uint8_t _gpu_fw_start[], _gpu_fw_end[];',
                            'extern uint8_t _gpu_fw_start[], _gpu_fw_end[];')
    source = re.sub(r'__asm__ volatile\([^;]+;', '', source)
    needle = '*(volatile uint32_t *)(uintptr_t)(base + off) = v;'
    assert source.count(needle) == 1
    source = source.replace(needle, needle + '\n mock_write(base + off, v);')
    with tempfile.TemporaryDirectory(prefix='fuxi-gpu-test-') as tmp:
        tmp = Path(tmp)
        fw = args.firmware.resolve() if args.firmware else tmp / 'fixture.bin'
        if not args.firmware:
            import struct
            fw.write_bytes(struct.pack('<4I', 0x10007fe0, 0, 1, 32) +
                           struct.pack('<4I', 0x10007fe0, 32, 0, 0) +
                           struct.pack('<I', 0x20010005) + b'\0'*28)
        asm = tmp / 'fw.S'
        import json
        asm.write_text('.section .data\n.global _gpu_fw_start, _gpu_fw_end\n_gpu_fw_start:\n.incbin ' +
                       json.dumps(str(fw)) + '\n_gpu_fw_end:\n.section .note.GNU-stack,"",@progbits\n')
        cfile = tmp / 'test.c'
        cfile.write_text(PREFIX + source + SUFFIX)
        exe = tmp / 'test'
        subprocess.run(['clang', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-DFUXI_XNU_LOADER=1', '-DEXPECT_BLOCKS=' + ('4' if args.firmware else '1'),
                        '-DEXPECT_PREALLOC=' + ('11' if args.firmware else '1'),
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(cfile), str(asm), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, cwd=tmp)
        subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(tmp / 'report.bin')],
                       check=True, stdout=subprocess.DEVNULL)
        print('PASS: six-page offline Python decoder')


if __name__ == '__main__':
    main()
