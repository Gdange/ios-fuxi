# Lane: GPU (Adreno A740 on Fuxi) — milestone 1

Goal of milestone 1: power up the A740 and prove the command processor (CP)
executes one command (CP_MEM_WRITE of 0x5e9a740 into a scratch page) under XNU
on Fuxi, using UniXNU / UniXNU-sheng as the reference. This file records the
Fuxi-verified facts, the staged loader probe delivered so far, how to read its
report, and the milestone-2+ plan.

Reference tree (sheng, SM8550, same SoC family, reached the iOS desktop with
the GPU): `research/UniXNU-sheng/package/GpuPowerProbe.cpp` +
`GpuPowerBoard.h`, `ShengGmu.c`, `ShengGmuVotes.c`, `ShengGmuCommandDb.c`;
driver kits `research/UniXNU/drivers/NativeAdreno/*`, `kits/ArmSmmu4K/*`,
`kits/AdrenoPM4/*`. Linux pins: ianchb/sm8550-mainline `42f3b40c`
(msm/adreno + qcom clk/cmd-db/arm-smmu).

Status: stages 1–7 (read-only snapshot → GMU clocks on → cmd-db parse) are
implemented as a bare-metal loader probe and build clean. Stages 8–14 (GMU
firmware load/HFI, GPU SMMU context, SQE + hw_init, ring + CP_ME_INIT +
CP_MEM_WRITE) are specified below as the milestone-2 driver port; they need the
heavyweight SMMU-4K / HFI / NativeAdreno bodies and were not bare-metal-ported.

Branch: `worktree-agent-aa64be1bb2bdb0b08` (base commit f6d6b63). See the
tail of this file for the lane commit id once committed.

---

## 1. Fuxi-verified GPU facts (facts vs. guesses)

Source of "live" facts: `research/device/probe-09/live.dtb` (the booted Fuxi
DTB) read with `tools/fuxi-inspect-dtb.py`. Register maths: pinned Linux
`42f3b40c`. "sheng" = the reference tablet; differences are called out.

### Identity / memory map (FACT, live DT)
| Item | Value | Source |
|------|-------|--------|
| GPU node | `qcom,kgsl-3d0@3d00000`, compatible `qcom,adreno-gpu-gen7-2-1` (A740 v2) | live DT |
| chip_id | `0x43050a01` ("C510v2") | live DT + a6xx_catalog.c (same as sheng) |
| GPU reg window | `0x03d00000` size `0x40000` | live DT |
| gpucc | `0x03d90000` size `0xa000` (qcom,kalama-gpucc) | live DT |
| GCC | `0x00100000` (kalama-gcc-v2); gpu branch page `0x171000`, gpll0 vote `0x152000` | live DT + gcc-kalama.c |
| adreno SMMU | `kgsl-smmu@3da0000` size `0x40000`, `qcom,qsmmu-v500`+`qcom,adreno-smmu`, dma-coherent, 3-lvl | live DT |
| GMU node | `qcom,gmu`, reg `gmu`=`0x3d68000/0x37000`, `gmu_pdc`=`0xb280000/0x10000`, `gmu_ao_blk_dec0`=`0x3d40000/0x10000` | live DT |
| GMU core base used by probe | `0x3d6a000` (upstream/sheng); Fuxi's `0x3d68000/0x37000` node **contains** it — same registers, same absolute addresses | note |
| rscc | `0x3d50000`; cx_misc/cx_mem `0x3d9e000` | live DT / sheng |
| cmd-db reserved region | `aop_cmd_db_region@81c60000` reg `0x81c60000` size `0x20000`, `no-map` | **live DT (FACT)** — matches sheng `0x81c60000/0x20000` |
| zap reserved region | `gpu_micro_code_region@9b09a000` size `0x2000`, `no-map` | live DT (same as sheng) |

### SMMU stream IDs / banks (FACT, live DT)
- `kgsl-iommu` context banks: `gfx3d_user` SID 0, `gfx3d_lpac` SID 1,
  `gfx3d_secure` SID 2 (mask 0). GMU: `iommus = <… SID 5>`.
- `qcom,actlr = <0 0x3ff 0x32b>`; QTB `gpu_qtb@3de8000` stream-id-range `0 0x400`.
- Driver-chosen (from sheng, GUESS-but-matches-arm-smmu-qcom): GPU uses **bank 0,
  ASID 2**, SMR slots 0/1 → SIDs 0/1; GMU uses **bank 1, ASID 1, SMR slot 2 → SID 5**.
  The actual free bank/slot is confirmed by the stage-4 SMMU snapshot on device.

### IRQs / clocks (FACT, live DT)
- GPU IRQ SPI 300 level. GMU: hfi SPI 304, gmu SPI 305.
- gpucc GDSCs: `gpu_cc_cx_gdsc` gdscr `0x9108` (gds_hw_ctrl `0x953c`, VOTABLE,
  RETAIN_FF), `gpu_cc_gx_gdsc` gdscr `0x905c` (owned by the GMU — never touched
  by CPU here). Branch/PLL/RCG offsets confirmed against gpucc-sm8550.c:
  ahb `0x911c`, cx_gmu `0x913c`, hlos smmu vote `0x7000`, cxo_aon `0x9004`,
  demet `0x900c`, cxo `0x9144`, hub_cx_int `0x9148`, gmu RCG `0x9318`,
  hub RCG `0x93ec`, PLL0 `0x0`, PLL1 `0x1000`.
- GCC gpu branches (page `0x71000`): cfg_ahb `+0x004` (critical, hwcg), memnoc
  `+0x010`, snoc_dvm `+0x018`, ddrss_gpu_axi `+0x154`; gpll0 votes in `0x52000` bits 15/16.

### Power levels (FACT, live DT `gpu-pwrlevels-0`, initial level 8 = 220 MHz)
9 levels, freq/level(ARC): 719.6 MHz/0xe0, 680/0xc0, 615/0x90, 550/0x80,
475/0x50, 401/0x40, 348/0x3c, 295/0x38 (corrected 2026-10-20 from live DT; bin not yet confirmed), 220/0x34. GMU opp table: 500 MHz/SVS,
200 MHz/LOW_SVS. gmu-freq-table: 220 MHz vote 0x40, 550 MHz vote 0x80.
(Upstream sm8550.dtsi GPU opp table differs slightly in top bins; Fuxi's own
`gpu-pwrlevels-0` above is authoritative and is what the perf table must use.)

### Firmware (stored, NOT committed — under `research/firmware/gpu/`, gitignored)
| File | SHA256 | Bytes | Source |
|------|--------|-------|--------|
| a740_sqe.fw (sheng, **default**) | `6d38046654333a6a5a4ee77ab67abc24e3c0c5c68237513afe98e94da8fc9fcc` | 75380 | github ianchb/sheng-firmware master |
| gmu_gen70200.bin | `1a2a419c39046d3141fc5fed5aa7f971de2db40cc7a1d89693c3e26fad64dd98` | 66844 | ianchb/sheng-firmware |
| a740_sqe.fw (linux-firmware, newer) | `96fee336424b139100fc60b5b45a907360e4b3936d7e1d00406b9bd80ca48473` | 76724 | gitlab kernel-firmware main `30979b11` |
| gmu_gen70200.bin (linux-firmware) | identical to sheng | 66844 | " |

- SQE is copied into the ring's SQE BO **without its first dword**
  (`adreno_fw_create_bo`). GMU fw is the block format `{addr,size,type,value}`
  + payload: ITCM `{0,0x4000}`, icache `{0x4000,0xffc000}`, DTCM
  `{0x10004000,0x4000}`; the file is 11 zero-size prealloc headers + 4 data blocks (ITCM 0x0 0x1dc, icache 0x4000 0x10210, DTCM 0x10007f98 0x28, 0x10007fdc 0x18); prealloc needs the whole icache window backed (corrected 2026-10-20).
- Fuxi's own vendor blobs (`a740_sqe.fw`, `gmu_gen70200.bin`, and the signed
  `a740_zap.*`) are **not available locally** — no Fuxi vendor image was
  extracted. The zap step (milestone 2) needs a **Fuxi-signed** zap; sheng
  reached the desktop *without* the zap (`sheng-gpu-zap=0`), so milestone 1 and
  the first userspace attempt can skip it. GUESS: Fuxi's SQE/GMU blobs are
  byte-identical to the generic a740 ones (same chip_id), so the sheng/
  linux-firmware copies are expected to work; confirm by SHA against a Fuxi
  vendor image when one is dumped.

### Differences from sheng
- GMU DT node base `0x3d68000/0x37000` (Fuxi) vs `0x3d6a000/0x35000`
  (sheng/upstream) — same block, Fuxi's starts 0x2000 lower; absolute register
  addresses are identical, so the `dword*4 - 0x6a000` offset math is unchanged.
- cmd-db region, zap region, chip_id, SIDs: identical to sheng (confirmed FACT).
- Fuxi `gpu-pwrlevels-0` is the downstream 9-level table (above); build the HFI
  perf table from it, not from the upstream dtsi opp table.

---

## 2. The staged loader probe (delivered)

Files (tracked, additive, kept in their own TU so other lanes' probe.c edits
stay separate):
- `shim/fuxi-probe/gpu.c` — the staged probe (`gpu_probe(limit)`), `#include`d
  by `shim/fuxi-probe/probe.c` before `ud_setup()`.
- `shim/fuxi-probe/probe.c` — one added dispatch branch: USB control type `0xc0`
  request **`0x56`**, `wValue = 0x4753`, `wIndex = <stage 1..7>`, returns a
  fixed 512-byte (128 × u32) report. (Requests 0x40–0x55 were in use; 0x56 free.)

Environment: the fuxi-loader runs with **MMU and caches OFF, VA==PA**, so every
MMIO access is naturally uncached Device memory (correct for GPU/GMU bring-up
coherence) and every "window" is just a physical base. A bad MMIO raises an
external abort that resets the SoC, so each stage runs only if the previous one
proved its prerequisite, and the furthest stage reached (before a reset drops
enumeration) localises a fault to one stage. `wIndex` caps the last stage run.

| Stage | What it does | Writes? | Risk |
|-------|--------------|---------|------|
| 1 | Read GCC gpu branches + gpll0 vote; **gated** GPUCC GDSC/clock reads (only if cfg_ahb enable bit set); **gated** SMMU GR0 + SMR/S2CR scan (only if CX on + hlos vote clock on); **gated** GPU RBBM_STATUS (only if CX+GX on) | none | none — same windows the UFS/touch probes already read |
| 2 | Enable `gcc_gpu_cfg_ahb_clk` bit0 (the one write the gcc-sm8550 probe makes) | 1 | low |
| 3 | CX GDSC on (clear SW_COLLAPSE, poll gds_hw_ctrl PWR_ON, set RETAIN_MEM/PERIPH/FF) + `gpu_cc_ahb` + `hlos1_vote_gpu_smmu`; wait vote CLK_OFF clear | yes | medium (first power-domain write; GX never touched) |
| 4 | Re-read adreno SMMU GR0 + CB0/CB1 CBAR/SCTLR/FSR/TCR now that it is powered | none | reads behind CX power — gated on stage 3 success |
| 5 | GMU clock snapshot (PLL1 mode/opmode/L, gmu/hub RCGs, branches, axi, vote); + GMU core state words if AHB+cx_gmu already run | none | reads behind CX power |
| 6 | GMU clock bring-up (a6xx_gmu_resume + gpucc order): gpll0 votes → configure+enable PLL0/PLL1 (lucid_ole) → hub RCG 200 MHz (GPLL0 sel5 div5) → gmu RCG 220 MHz (PLL1 sel3 div3) → 8 branches → wait cx_gmu CLK_OFF; then read GMU cold-start preconditions | yes | medium (PLL enable + RCG switches; each sub-step has its own timeout + report) |
| 7 | Copy the cmd-db reserved region (DRAM) and parse the slave table; resolve gfx/mx/cx/gmxc.lvl + SH0/MC0/ACV/CN0 addresses | none (DRAM read) | low |

Image (deliverable): `shim/out/lane-gpu/fuxi-gpu-0092g-boot.img`
SHA256 `7cd7a1e240c65299021989823ed557ef50cc8f84d6dbba54a1db609d52a055e5`
(built from payload `shim/out/fuxi-0091d-payload.bin`; SHA recorded in
`shim/out/lane-gpu/SHA256SUMS`). The standalone probe image also builds:
`shim/fuxi-probe/build.sh` → `shim/out/fuxi-probe/fuxi-probe-boot.img`.

Build command (from the worktree):
```
CLANG=/hdd2/pixelos/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang \
LLD=.../ld.lld OBJCOPY=/usr/bin/llvm-objcopy-18 READELF=/usr/bin/llvm-readelf-18 \
NM=/usr/bin/llvm-nm-18 MKBOOTIMG=/hdd2/fuxi-ios/research/mkbootimg/mkbootimg.py \
bash shim/fuxi-loader/build.sh /hdd2/fuxi-ios/shim/out/fuxi-0091d-payload.bin
```

### Host tool + runner
- `tools/fuxi-gpu-probe.py` (tracked): finds the probe (1209:0001 'Fuxi hardware
  probe'), sends `0x56`, decodes the report stage by stage. `--stage N` (1..7),
  `--raw` dumps the 128 words.
- `research/run-gpu.py` (untracked, gitignored like run-touch.py): verifies the
  pinned iOS root is untouched in recovery, `fastboot boot`s the lane-gpu image,
  waits for enumeration, then runs the host tool in the `gbl-chainload-build`
  container. Usage: `python3 research/run-gpu.py <STAGE> [IMAGE]`.
  No handoff; the loader resets at its 90 s deadline (extend with request 0x54).

Procedure for the coordinator (serial device tests, phone cooling): run stage 1
first and confirm the read-only snapshot; climb one stage at a time. A SoC reset
(probe stops enumerating) between stages means the last stage touched a bad
register — the report of the prior stage is the evidence.

### Report word map (128 × u32, little-endian)
```
[0]  magic 0xa740     [1]  stage_limit
Stage 1/read-only:
[4] cfg_ahb [5] memnoc [6] snoc_dvm [7] gpll0_vote
[8] cx_gdscr [9] gx_gdscr [10] ahb_clk [11] smmu_vote [12] cx_gmu [13] gpucc_read
[14] gpu_read [15] smmu_read
[16] scr0 [17] id0 [18] id1 [19] id2 [20] gfsr
[21] smr_valid|(first<<16) [22] smr_first [23] s2cr_first [24] rbbm_status
Stage 2: [26] wrote_enable [27] cfg_ahb_after
Stage 3: [28] cx_gdscr_before [29] gds_hw_ctrl [30] cx_gdscr_after
         [31] ahb_clk [32] smmu_vote [33] smmu_on
Stage 4: [35] done [36] scr0 [37] id0 [38] id1 [39] id2
         [40..43] CB0 cbar/sctlr/fsr/tcr   [44..47] CB1 cbar/sctlr/fsr/tcr
Stage 5: [48] pll1_mode [49] pll1_opmode [50] pll1_L
         [51] gmu_rcg_cmd [52] gmu_rcg_cfg [53] hub_rcg_cmd [54] hub_rcg_cfg
         [55] cx_gmu [56] cxo [57] hub_cx_int [58] demet [59] axi [60] gpll0_vote
         [61] gmu_sysreset [62] cxmisc_tcm_ret [63] rscc_seq_busy [64] hfi_status
         [79] gmu_regs_read (1 = GMU core was readable in stage 5)
Stage 6: [65] result (0 not-run, 7 all-pass; 3 PLL1 lock t/o, 4 hub RCG t/o,
             5 gmu RCG t/o, 6 cx_gmu CLK_OFF t/o)
         [66] gpll0_voted [67] pll1_lock_polls [68] cx_gmu [69] cx_gmu_polls
         [70] pll1_mode [71] pll1_opmode [72] gmu_cfg(&0x7ff) [73] hub_cfg(&0x7ff)
         [74] gmu_sysreset(want 1) [75] tcm_ret(want 0) [76] rsc_busy(want 0)
         [77] hfi_status(want bit0=0)
Stage 7: [80] cmddb_version [81] cmddb_magic(want 0x0c0330db) [82] slaves
         [83..90] addresses: gfx.lvl, mx.lvl, cx.lvl, gmxc.lvl, SH0, MC0, ACV, CN0
```
A healthy stage-6 result is `[65]=7` with `[74..77] = 1/0/0/even`, i.e. the GMU
is powered, clocked and held in reset, RSC idle, HFI not yet ready — exactly the
cold-start precondition `ShengGmuLoadAndStartHfi` checks.

---

## 3. Milestone 2+ plan (stages 8–14) with effort estimates

These need the SMMU-4K page-table builder, the GMU firmware loader, the HFI
protocol and the NativeAdreno ring/CP bodies — all present in the reference
tree but assuming an allocator + IOMemoryMap, so they belong in the XNU driver
port, not the bare-metal probe. The full sequences are extracted and verified
(see the summary / reference files); estimates are engineer-days for this lane.

1. **GMU SMMU context (bank 1, ASID 1, slot 2 → SID 5)** + **GPU SMMU context
   (bank 0, ASID 2, slots 0/1 → SIDs 0/1)**. Port `kits/ArmSmmu4K` (stage-1
   translation, stage-2 bypass; CBAR 0x1f300, TCR `(2<<30)|walk|(64-bits)`,
   MAIR 0xff44, SCTLR 0x1067; TLBIALL/SYNC). Buffers + page tables in the high
   XNU window (0x8c0000000..+1 GiB) so the host can read them back via request
   0x47. GMU fixed VAs: icache @0x4000 (exec), dummy @0x60000000, debug
   @0x60400000, log/hfi above dummy. GPU IOVAs: SQE 0x100000, ring 0x200000,
   memptrs 0x300000, pwrup 0x301000, scratch 0x302000; 2 GiB aperture.
   Est. **3–4 d** (the kit is written; porting its allocator to the loader's
   flat allocator and proving coherence is the work).
2. **GMU cold start** (`ShengGmuLoadAndStartHfi`): verify preconditions (stage 6
   already shows them green), RSC/PDC setup (A740 variant: `0x80000021` hidden
   TCS, seqmem0_drv0 at `0x154`, 5 uCode words `0xeaaae5a0…0x0020edad`; PDC is
   AOP-resident on A7xx so only `PDC_GPU_ENABLE_PDC=0x80000001` /
   `SEQ_START_ADDR=0` at `gmu_pdc` 0xb280000), FALNEXT/FAL + TCM_RET=1, zero
   icache/dummy/debug/log, HFI init, load the 15 fw blocks + readback, write
   config (BOOT_CONFIG 2, QTBL, FENCE_RANGE `0x80000000|(0x32<<18)|0x8a0`, CM3_CFG
   0x4052, GENERAL_10 chipid 0x7020100, GENERAL_8 log|3, GBIF_QSB_SIDE0..3
   0x71620, SYS_BUS/ICACHE/DCACHE=1), release reset, poll FW_INIT_RESULT then
   HFI_CTRL_STATUS bit0. Est. **2–3 d**.
3. **HFI startup exchange** (`AdrenoHfiStartup`): queue table in the hfi BO
   (header + 3 queue headers), send PERF_TABLE (id 4, from Fuxi `gpu-pwrlevels-0`
   + ARC votes), BW_TABLE (id 3, `a740_build_bw_table`: SH0/MC0/ACV data
   0x40000000, CN0 0x40000000/0x60000001, from the stage-7 cmd-db addresses),
   ACD/IFPC off, CORE_FW_START (id 14), START (id 10). Packet header
   `(seq<<20)|(count<<8)|id`, pad to 4 words 0xfafafafa, ring HOST2GMU_INTR_SET.
   Est. **2–3 d**.
4. **GX on**: HFI GX_BW_PERF_VOTE (id 30, {1,1,1}), then OOB GPU_SET
   (HOST2GMU bit30, ack GMU2HOST bit31, clear), verify
   GMU_SPTPRAC_PWR_CLK_STATUS bits0,1 clear. Est. **1 d**.
5. **SQE + GPU hw_init** (`NativeAdrenoA740HwInit.h` tables already derived:
   hwcg 54 regs, protect 46 regs): GBIF release, SECVID TSB, hwcg, QOS, UCHE
   trap/GMEM 16M..+3M, CP_PROTECT, APRIV 0x6f/0x0f/0x0f, UBWC (sm8550), INT_0_MASK
   0x33d283c2, SQE_INSTR_BASE. Est. **2 d**.
6. **Ring + CP_ME_INIT + CP_MEM_WRITE** (`NativeAdrenoRing`, `CpInit`, `AdrenoPM4`
   already present): CP_RB_CNTL (32 KiB, block 32), BV_RB_RPTR_ADDR, SQE_CNTL=1,
   a7xx pwrup reglist (16 regs, derived), CP_ME_INIT (opcode 0x48, mask
   `BIT0|1|3|6|8`, 3 contexts, 0x20000000, 0x2, pwrup iova, BIT31), fence
   (TYPE4 scratch + TYPE7 CACHE_CLEAN event), then CP_MEM_WRITE (opcode 0x3d:
   scratch iova lo/hi, data 0x5e9a740) + fence → **milestone-1 success word**.
   Est. **2 d**.

Milestone-1-complete (the CP executes one command) = items 1–6, roughly
**12–17 engineer-days** once the SMMU/HFI/ring bodies are wired to the XNU
driver's allocator. Zap/secure-mode is optional (sheng skipped it) and needs a
Fuxi-signed blob (dump a Fuxi vendor image first).

Milestone 3 (userspace/Metal) is a separate, larger effort: port
NativeAdrenoAccelerator + the IOAccelerator/IOGPU user client so
IOAcceleratorES / QuartzCore matches the published service. **Ghidra is
available** at `/ssd1/tools/ghidra_12.1.3_PUBLIC` (headless
`support/analyzeHeadless`) and is the right tool for: the Fuxi-signed zap blob
layout and QuartzCore/Metal plugin loading + IOAcceleratorES matching rules in
the iOS 16.7 dyld shared cache at `research/dsc`. Keep Ghidra projects under
scratch/worktree, never in git. (Not needed for milestone 1.)

---

## 4. Branch + commits
- Branch: `worktree-agent-aa64be1bb2bdb0b08`, base `f6d6b63`.
- Lane commit: recorded on commit (see `git log` for the "Lane GPU 0092g" entry).
- Deliverable image + SHA: `shim/out/lane-gpu/fuxi-gpu-0092g-boot.img`
  `7cd7a1e240c65299021989823ed557ef50cc8f84d6dbba54a1db609d52a055e5`.
