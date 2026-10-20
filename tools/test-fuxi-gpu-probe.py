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
extern uint8_t _gpu_fw_start[], _sqe_fw_start[], _sqe_fw_end[];
static uint64_t ticks;
#define rsr(reg) (strcmp(reg,"cntfrq_el0") ? ++ticks : 1000000ull)
static void delay(unsigned us) { ticks += us; }
static void mock_write(uint64_t at, uint32_t val);
'''
SUFFIX = r'''
static int firmware_success = 1, ack_mode, corrupt_readback, tlb_busy;
static unsigned firmware_releases, mmio_writes;
static int cp_mode; /* 1 stalled, 2 fault, 3 missing fence, 4 corrupt fence guard */
static uint64_t abort_read, abort_write;
static unsigned drop_sqe_base, start_fault, doorbells;
uint32_t probe_fault_esr;
uint64_t probe_fault_far;
uint32_t probe_safe_rd32(uint64_t pa, uint32_t *faulted)
{
 *faulted = pa == abort_read;
 if (*faulted) { probe_fault_esr=0x96000010; probe_fault_far=pa; return 0xdeadfa17; }
 if (pa == GPU_REG_BASE+G3_STAT_DATA*4)
  return 0xa7400000u | *(volatile uint32_t *)(uintptr_t)(GPU_REG_BASE+G3_STAT_ADDR*4);
 return *(volatile uint32_t *)(uintptr_t)pa;
}
unsigned probe_safe_wr32(uint64_t pa, uint32_t v)
{
 if (pa == abort_write) { probe_fault_esr=0x96000050; probe_fault_far=pa; return 1; }
 g_wr(pa, 0, v); return 0;
}
static uint32_t *ptr(uint64_t at) { return (uint32_t *)(uintptr_t)at; }
static void mock_write(uint64_t at, uint32_t val)
{
 mmio_writes++;
 if (drop_sqe_base && at == GPU_REG_BASE+G3_SQE_BASE*4) *ptr(at)=0;
 if (start_fault && at == GPU_REG_BASE+G3_SQE_CNTL*4 && val==1) {
  *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x58)=0x402;
  *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x68)=0xb0041;
  *ptr(GPU_SMMU_BASE+g_stride+0x400+g3_bank*4)=1;
  *ptr(GPU_REG_BASE+0x823*4)=0x80;
 }
 if (at == GPU_REG_BASE+0x807*4 && val) {
  doorbells++;
  assert(gpu_extra[5][7] && g3_bank != g_bank && g3_bank != 0);
  assert(*ptr(GPU_REG_BASE+0x808*4)==1 && *ptr(GPU_REG_BASE+0x802*4)==0x20c);
  assert(*ptr(GPU_REG_BASE+0x830*4)==G3_SQE_VA);
  assert(*ptr(G3_RING_PA)==0x70970001 && *ptr(G3_RING_PA+4)==1u<<27);
  assert(*ptr(G3_RING_PA+8)==0x70c80004 && *ptr(G3_RING_PA+12)==0x4b);
  if (cp_mode == 1) return;
  if (cp_mode == 2) { *ptr(GPU_REG_BASE+0x823*4)=4; return; }
  *ptr(GPU_REG_BASE+0x806*4)=val; *ptr(G3_PTR_PA)=val;
  if (val==G3_FENCE_DWORDS) {
   assert(*ptr(G3_RING_PA+G3_INIT_DWORDS*4)==0x703d8003);
   assert(*ptr(G3_RING_PA+(G3_INIT_DWORDS+1)*4)==G3_FENCE_VA && !*ptr(G3_RING_PA+(G3_INIT_DWORDS+2)*4));
   if (cp_mode != 3) *ptr(G3_FENCE_PA)=*ptr(G3_RING_PA+(G3_INIT_DWORDS+3)*4);
   if (cp_mode == 4) *ptr(G3_FENCE_PA+4)=0xbad;
  }
 }
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
 drop_sqe_base=start_fault=doorbells=0;
 memset((void *)(uintptr_t)GPU_REG_BASE, 0, 0x100000);
 memset((void *)(uintptr_t)G_MEM, 0, G_MEM_SIZE);
 memset(gpu_report, 0, sizeof(gpu_report)); memset(gpu_extra, 0, sizeof(gpu_extra));
 gpu_snapshot_valid = gpu_gmu_started = gpu_m2_attempted = gpu_s8_ok = 0;
 gpu_s9_attempted = gpu_s9_ok = gpu_s10_attempted = 0;
 ticks = firmware_releases = mmio_writes = 0;
 cp_mode=0; abort_read=abort_write=0;
 ack_mode = corrupt_readback = tlb_busy = 0; firmware_success = 1;
 *ptr(GPU_SMMU_BASE+0x20) = 0x4c017e18;
 *ptr(GPU_SMMU_BASE+0x24) = 0x40000016;
 *ptr(GPU_SMMU_BASE+0x28) = 0x5511;
 *ptr(GPU_SMMU_BASE) = 0x2f0406;
 *ptr(GPU_GMU_CORE+0x5000*4) = 1;
 *ptr(GPU_GMU_CORE+(0x1c00+0xff8)*4) = 0x20010005;
 gpu_extra[0][0] = 0xa7400004;
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
 /* m3: two GPU streams, separate tables/bank/ASID; GMU memory untouched. */
 uint64_t gmu_root=*(uint64_t *)(uintptr_t)G_MEM;
 gpu_m2(11); assert(gpu_extra[5][2]==1 && g3_bank==2 && g3_asid==2);
 assert(g3_slot[0]==1 && g3_slot[1]==2 && gpu_extra[5][33]==3);
 assert(*(uint64_t *)(uintptr_t)G_MEM==gmu_root && g_bank==1 && g_asid==1);
 for (unsigned v=G3_SQE_VA;v<G3_FENCE_VA+4096;v+=4096) {
  uint64_t *l1=(void *)(uintptr_t)G3_PT;
  uint64_t *l2=(void *)(uintptr_t)(l1[v>>30]&~0xfffull);
  uint64_t *l3=(void *)(uintptr_t)(l2[(v>>21)&511]&~0xfffull);
  uint64_t pte=l3[(v>>12)&511];
  assert((pte&~0xfffull)==G3_SQE_PA+v-G3_SQE_VA);
  assert((pte&0xc0)==(v<G3_RING_VA?0xc0:v==G3_PTR_VA?0:0x40));
 }
 unsigned tail=(G3_FENCE_VA>>12)&511;
 uint64_t *root=(void *)(uintptr_t)G3_PT;
 uint64_t *mid=(void *)(uintptr_t)(root[1]&~0xfffull);
 uint64_t *leaf=(void *)(uintptr_t)(mid[0]&~0xfffull);
 assert(!root[0] && !root[2] && !leaf[tail+1] && !mid[1]);
 gpu_m2(12); assert(gpu_extra[5][3]==1 && gpu_extra[5][49]==G3_INIT_DWORDS && gpu_extra[5][43]);
 assert(gpu_extra[5][17]==_sqe_fw_end-_sqe_fw_start && gpu_extra[5][18]==_sqe_fw_end-_sqe_fw_start-4);
 assert(!memcmp((void *)(uintptr_t)G3_SQE_PA,_sqe_fw_start+4,gpu_extra[5][18]));
 gpu_m2(13); assert(gpu_extra[5][4]==1 && gpu_extra[5][53]==G3_MAGIC && gpu_extra[5][56]==G3_FENCE_DWORDS);
 assert(gpu_extra[7][3]==3 && gpu_extra[7][4]==32 && gpu_extra[7][63]==51);
 for (unsigned i=0;i<51;i++) assert(gpu_extra[7][64+i]==(0xa7400000u|i));
 before=mmio_writes; gpu_probe(13); gpu_probe(11); assert(before==mmio_writes);
 assert(gpu_extra[6][0]==12 && gpu_extra[6][64]==13 && gpu_gmu_started);
 FILE *f=fopen("report.bin","wb"); assert(f); gpu_report[0]=0xa740;
 fwrite(gpu_report,1,512,f); fwrite(gpu_extra,1,(G_PAGES-1)*512,f); fclose(f);
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
 /* GPU ownership: masked SID collision, no bank, snapshot fault, timeout. */
 reset_model(); aux_fixture();
 *ptr(GPU_SMMU_BASE+SMMU_SMR(3))=0x80000000;
 assert(g_snapshot()); gpu_m2(13);
 assert(gpu_extra[5][2]==0xb03 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10);
 for (unsigned b=2;b<g_ncb;b++) *ptr(GPU_SMMU_BASE+g_cb(b))=1;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb04 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10); gpu_extra[1][51]=1u<<3;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb01 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10); tlb_busy=1;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb06 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10); corrupt_readback=1;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb07 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(11);
 *ptr(GPU_SMMU_BASE+g_cb(g_bank)+0x58)=0x400;
 *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x58)=0x400;
 gpu_m2(13); assert(gpu_extra[5][4]==1); /* reserved FSR bit10 is evidence only */
 /* Failed stages are never retried and never unlock the XNU handoff. */
 for (unsigned mode=1;mode<=4;mode++) {
  reset_model(); aux_fixture(); gpu_m2(11); cp_mode=mode;
  unsigned begun=ticks; gpu_m2(13);
  assert(ticks-begun<100000);
  if (mode<=2) assert(gpu_extra[5][3]==(mode==1?0xc07:0xc06) && !gpu_extra[5][6]);
  else assert(gpu_extra[5][4]==(mode==3?0xd03:0xd05));
  before=mmio_writes; gpu_m2(13); assert(before==mmio_writes && gpu_gmu_started);
 }
 reset_model(); aux_fixture(); gpu_m2(11); abort_write=GPU_REG_BASE+0xf400*4;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc09 && gpu_extra[5][116] && !gpu_extra[5][6]);
 reset_model(); aux_fixture(); gpu_m2(10); abort_read=GPU_SMMU_BASE+SMMU_SMR(1);
 gpu_m2(13); assert(gpu_extra[5][2]==0xb01 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(10);
 *ptr(GPU_SMMU_BASE+SMMU_SMR(1))=0x80000000;
 gpu_m2(13); assert(gpu_extra[5][2]==0xb02 && !gpu_extra[5][5]);
 reset_model(); aux_fixture(); gpu_m2(11); tlb_busy=0;
 *ptr(GPU_SMMU_BASE+g_cb(g3_bank)+0x58)=2;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc06 && !gpu_extra[5][6]);
 /* Dropped programming is diagnosed before any SQE execution/doorbell. */
 reset_model(); aux_fixture(); gpu_m2(11); drop_sqe_base=1;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc0b && gpu_extra[5][94]==1);
 assert(!gpu_extra[5][59] && !doorbells && gpu_extra[7][3]==1);
 /* Reproduce the device signature at SQE RUN with an empty ring. The
  * capture must distinguish this from CP_ME_INIT, which cannot run yet. */
 reset_model(); aux_fixture(); gpu_m2(11); start_fault=1;
 gpu_m2(13); assert(gpu_extra[5][3]==0xc06 && gpu_extra[5][60]==7);
 assert(!doorbells && gpu_extra[7][3]==2 && gpu_extra[7][5]==0x80);
 assert(gpu_extra[7][6]==0x402 && gpu_extra[7][40]==1);
 assert(!gpu_extra[5][6] && gpu_extra[6][2]==0);
 puts("PASS: SMMU snapshot/ownership/readback, all icache PTEs, firmware load, HFI order/ACK/wrap/errors/timeouts, m3 mappings/CP packets/fence/abort/timeouts, one-shot GMU/handoff latch");
 return 0;
}
'''


def audit_linux(msm):
    """Compare the hand-copied tables/critical offsets to the supplied source."""
    xml = (msm / 'registers/adreno/a6xx.xml').read_text()
    catalog = (msm / 'adreno/a6xx_catalog.c').read_text()
    header = (ROOT / 'shim/fuxi-probe/gpu-a740.h').read_text()
    offsets = {name: int(off, 16) for off, name in re.findall(
        r'<reg(?:32|64) offset="(0x[0-9a-fA-F]+)" name="([^"]+)"', xml)}
    def body(text, name):
        return re.search(r'\b' + name + r'\[.*?=\s*\{(.*?)\n\};', text, re.S)[1]
    local = [(int(off, 16), int(val, 16)) for off, val in re.findall(
        r'\{(0x[0-9a-f]+), (0x[0-9a-f]+)\}', body(header, 'g3_hwcg'))]
    linux = [(offsets[name], int(val, 16)) for name, val in re.findall(
        r'\{ REG_A[67]XX_(\w+), (0x[0-9a-f]+) \}', body(catalog, 'a740_hwcg'))]
    assert len(local) == 54 and local == linux, 'A740 HWCG offset/value drift'
    protect = re.sub(r'/\*.*?\*/', '', body(catalog, 'a730_protect_regs'), flags=re.S)
    expected = []
    for line in protect.splitlines():
        match = re.search(r'A6XX_PROTECT_(RDONLY|NORDWR)\((0x[0-9a-f]+), (0x[0-9a-f]+)\)', line)
        if match:
            mode, base, count = match.groups()
            expected.append(int(base, 16) | int(count, 16) << 18 |
                            (0x80000000 if mode == 'NORDWR' else 0))
        elif line.strip() == '0,':
            expected.append(0)
    local_protect = re.sub(r'/\*.*?\*/', '', body(header, 'g3_protect'), flags=re.S)
    actual = [int(x.strip(), 0) for x in local_protect.split(',') if x.strip()]
    assert len(actual) == 48 and actual == expected, 'A740 protection table drift'
    names = {'G3_RB_BASE': 'CP_RB_BASE', 'G3_RB_CNTL': 'CP_RB_CNTL',
             'G3_RPTR_ADDR': 'CP_RB_RPTR_ADDR', 'G3_SQE_CNTL': 'CP_SQE_CNTL',
             'G3_SQE_BASE': 'CP_SQE_INSTR_BASE', 'G3_APRIV': 'CP_APRIV_CNTL',
             'G3_STAT_ADDR': 'CP_SQE_STAT_ADDR', 'G3_STAT_DATA': 'CP_SQE_STAT_DATA',
             'G3_APERTURE_HOST': 'CP_APERTURE_CNTL_HOST',
             'G3_BV_STAT_ADDR': 'CP_BV_SQE_STAT_ADDR', 'G3_BV_STAT_DATA': 'CP_BV_SQE_STAT_DATA',
             'G3_BV_RPTR_ADDR': 'CP_BV_RB_RPTR_ADDR',
             'G3_AC_STAT_ADDR': 'CP_SQE_AC_STAT_ADDR', 'G3_AC_STAT_DATA': 'CP_SQE_AC_STAT_DATA',
             'G3_LPAC_CNTL': 'CP_LPAC_SQE_CNTL'}
    for macro, name in names.items():
        value = int(re.search(r'^#define ' + macro + r'\s+(0x[0-9a-f]+)', header, re.M)[1], 16)
        assert value == offsets[name], f'{macro}: Linux {name} offset drift'
    print('PASS: Linux source audit: 54 HWCG pairs, 48 protection slots, 15 CP offsets')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--sqe', type=Path, help='external local a740_sqe.fw')
    ap.add_argument('--firmware', type=Path, help='external local gmu_gen70200.bin; never copied into git')
    ap.add_argument('--linux-msm', type=Path, help='supplied drivers/gpu/drm/msm source directory for register/table audit')
    args = ap.parse_args()
    if args.linux_msm:
        audit_linux(args.linux_msm)
    source = (ROOT / 'shim/fuxi-probe/gpu.c').read_text()
    source = source.replace('extern const uint8_t _gpu_fw_start[], _gpu_fw_end[];',
                            'extern uint8_t _gpu_fw_start[], _gpu_fw_end[];')
    source = source.replace('extern const uint8_t _sqe_fw_start[], _sqe_fw_end[];',
                            'extern uint8_t _sqe_fw_start[], _sqe_fw_end[];')
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
        sqe = args.sqe.resolve() if args.sqe else tmp / 'sqe.bin'
        if not args.sqe:
            sqe.write_bytes(bytes.fromhex('000000000102030405060708'))
        asm = tmp / 'fw.S'
        import json
        asm.write_text('.section .data\n.global _gpu_fw_start, _gpu_fw_end\n_gpu_fw_start:\n.incbin ' +
                       json.dumps(str(fw)) + '\n_gpu_fw_end:\n.global _sqe_fw_start, _sqe_fw_end\n_sqe_fw_start:\n.incbin ' +
                       json.dumps(str(sqe)) + '\n_sqe_fw_end:\n.section .note.GNU-stack,"",@progbits\n')
        cfile = tmp / 'test.c'
        cfile.write_text(PREFIX + source + SUFFIX)
        exe = tmp / 'test'
        subprocess.run(['clang', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'shim/fuxi-probe'), '-DFUXI_XNU_LOADER=1', '-DEXPECT_BLOCKS=' + ('4' if args.firmware else '1'),
                        '-DEXPECT_PREALLOC=' + ('11' if args.firmware else '1'),
                        '-fsanitize=undefined', '-fno-sanitize-recover=all',
                        str(cfile), str(asm), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, cwd=tmp)
        report = subprocess.run(['python3', str(ROOT / 'tools/fuxi-gpu-probe.py'), '--decode', str(tmp / 'report.bin')],
                                check=True, capture_output=True, text=True).stdout
        assert 'SQE_STAT count=51' in report and 'BR=0xa7400000/a7400001' in report
        assert '70c80004 0000004b' in report and 'SQElo: before_RUN=0x40000000' in report
        import runpy
        decoder = runpy.run_path(str(ROOT / 'tools/fuxi-gpu-probe.py'))
        client = decoder['fault_client']
        assert client(0, 0) == 'CP' and client(3, 9) == 'BV_SP' and client(8, 7) == 'LPAC_TP'
        assert decoder['bit_names'](0x80, decoder['CP_BITS']) == 'BR_ILLEGAL_INSTR'
        print('PASS: ten-page offline Python decoder')


if __name__ == '__main__':
    main()
