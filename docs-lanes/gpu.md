# Lane: GPU A740 — milestone 2 implementation, 2026-10-20

Branch `codex-gpu-m2`, base `43278d1`. Stages 8a/8/9/10 are implemented and
validated offline; **none has been run on the phone**. The coordinator owns all
device tests. No phone, USB, adb or fastboot access was used in this lane.
`NOTES.md` is untouched by explicit task instruction; this file is the handoff.
Commit: the commit containing this handoff (`git log -1` on the lane).

Stages 1–7 passed on Fuxi (coordinator runs gpu-01, gpu-03..07): CX on,
PLL1 locked after 6 polls, GMU reset=1, TCM retention=0, RSC idle, HFI not ready.
SMMU ID0/1/2 = `0x4c017e18 / 0x40000016 / 0x5511` (24 SMRs, 22 CBs,
4K register pages, CB base +0x20000, 36-bit OAS); sCR0 `0x002f0406`.
These are observations copied from the handoff, not new device results.

Reference: the supplied Linux msm tree under
`/hdd2/googlebookos-port/artifacts/source/kernel-guest-3a7d1771d492/drivers/gpu/drm/msm/`,
`adreno/a6xx_gmu.c` (memory_probe/fw_load/fw_start/start/rpmh_init),
`a6xx_hfi.c` + `.h` (queue layouts, messages, ARC votes, A740 BW table),
`a6xx_gpu.c` (OOB lifetime), `registers/adreno/a6xx_gmu.xml`, plus
`drivers/iommu/arm/arm-smmu/arm-smmu.{c,h}` and `drivers/soc/qcom/cmd-db.c`.
The supplied source has A740 support beyond vanilla upstream v6.12; its local
register definitions are authoritative for this port. Upstream comparison:
[Linux v6.12 GMU](https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/msm/adreno/a6xx_gmu.c).
UniXNU-sheng is a secondary register cross-check, not a device result for Fuxi.

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
  This was a reference assumption, not a device observation. Milestone 2
  selects an unused disabled bank, a free SMR and an unused ASID from stage 8a;
  bank 0 is deliberately left for the GPU. Existing SID5 matches cause refusal.

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
  extracted. Any future zap step would need a **Fuxi-signed** zap; sheng
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

## 2. Stages and USB interface

GPU code stays in `shim/fuxi-probe/gpu.c`, included by `probe.c`. The dispatch
changes only add page selection and the XNU handoff guard. Loader MMU/caches
are off, VA==PA. CPU accesses are uncached; every publication has barriers.
A bad MMIO may externally abort/reset, so software deadlines cannot recover it.

| Stage / wIndex | Operation | Risk / gating |
|---|---|---|
| 1 | Existing gated GCC/GPUCC/SMMU/GPU snapshot | Read-only, verified |
| 2 | Enable GCC cfg_ahb | Low, verified |
| 3 | CX GDSC + AHB/SMMU vote clocks | Medium, verified |
| 4 | Powered SMMU globals + CB0/1 | Read-only, verified |
| 5 | GMU clock/reset snapshot | Read-only, verified |
| 6 | PLL/GMU/hub clocks, cold prerequisites | Medium, verified |
| 7 | cmd-db snapshot/resource lookup | Read-only, verified |
| **8a / 0x008a** | Snapshot all 24 SMR/S2CR pairs and all 22 CBs with CX on | Low additional risk: new register reads; preceding 2/3/6 still write |
| **8** | Validate firmware/prealloc extents, select unused context/slot/ASID, build 4K tables in fixed RAM, map SID5, FAL veto + TCM retention, load/read back ITCM/DTCM/icache | High: first SMMU programming and about 17 MiB of scratch writes; no CM3 start |
| **9** | Build cmd-db ARC votes, RSC/PDC init, HFI table, cold GMU config/start, init wait, HFI ready, PERF -> BW -> CORE_FW_START -> START ACKs | High: firmware runs and accesses RAM; permanent handoff lock |
| **10** | Blocking low-frequency vote, GPU_SET OOB, wait GX, read RBBM_STATUS, release OOB | High: first GX activation; requires stages8/9 complete |

Request: type `0xc0`, request `0x56`, wValue `0x4753`. Stages1..10 and `0x8a`
run through the requested limit and return page0 (512 bytes). Invalid indices
stall. **Stage8a requires stage7 valid**, and 8–10 require its powered snapshot.
A failed stage never advances. After 8a the snapshot is frozen; each mutating
stage8/9/10 is attempted at most once. Later limits may advance the sequence;
repeating a failed stage requires a fresh boot. Requests for stages1..7 after
8a return the existing snapshot, avoiding PLL/TCM reinitialization.

Read-only report fetches: wIndex **0x0100..0x0105** select pages0..5. They only
copy RAM, do not run stages or refresh MMIO. All unused words are zero. Snapshot
pages2..5 are from immediately before the first stage8 write. Page1 live fields
are sampled at the end of each stage8a/8/9/10 request, not on page fetch.

**XNU request 0x49 is refused (USB stall) from immediately before CM3 reset is
released onward, including init timeout, ACK failure and subsequent requests.**
There is no unload/reset operation that clears this latch. Retain tables and
buffers until the normal loader reset. No zap/secure mode, SQE, GPU user SIDs,
CP, ring, rendering or XNU GPU driver is implemented by this milestone.

Timeouts: TLB sync 1000 polls × 1 us; firmware/HFI/GX init each 1000 × 10 us;
each HFI ACK has one 50 ms counter deadline, including all receive handling.
MMIO overhead adds elapsed time; all loops remain bounded. Firmware is validated
in full before any buffer/context write, and every data dword is read back.
Malformed responses, unsolicited ACKs, firmware errors and queue corruption stop
startup rather than being drained without a bound.

## 3. Fixed loader-only DRAM ownership

Reserve **[0x8f0000000, 0x8f2000000)** (32 MiB), inside the already proven
`[0x8c0000000, 0x900000000)` XNU RAM window. The loader has not copied XNU into
this window while these probes run. The runtime checks this reservation against
`_head.._end` (including embedded payload/stacks/USB state) and the incoming FDT
before writing it. No storage or partition operation is involved. It is a fixed
loader scratch reservation, **not a Linux DT reserved-memory allocation**.

| Use | GMU VA | Physical base | Bytes |
|---|---:|---:|---:|
| SMMU table pool | not mapped | 0x8f0000000 | 0x100000 reserved; 13 pages used |
| icache (privileged RW/execute) | 0x00004000 | 0x8f0100000 | 0xffc000 |
| dummy (privileged RW) | 0x60000000 | 0x8f1100000 | 0x2000 |
| log (RW, unprivileged allowed) | 0x60002000 | 0x8f1102000 | 0x4000 |
| HFI (RW, unprivileged allowed) | 0x60006000 | 0x8f1106000 | 0x4000 |
| debug (privileged RW) | 0x60400000 | 0x8f110a000 | 0x7000 |

Unmapped holes stay unmapped. No large `.bss` buffers are added (the linker puts
`.bss` before `.payload`). The existing 128 KiB cmd-db copy remains in `.bss`.
Three-level 4K tables cover 32-bit IOVA/36-bit PA. Leaves use normal noncacheable
MAIR index0=0x44 with inner sharing and AF; walks are noncacheable/outer shared.
CBAR=0x1f300 (stage1 translation, stage2 bypass), CBA2R=1,
TCR2=0x38011, TCR=0x80802020, MAIR0=0xff44, SCTLR=0x1067.
Readback permits only the reference-observed TCR2 TBI bits5/6 reading one and
SCTLR ASIDPNE bit12 reading zero; all other configured bits must match.
TTBR0 = root PA | ASID<<48; TTBR1 disabled. TLBASID + bounded TLBSYNC precedes
M=1, then S2CR translate/default privilege and exact SMR SID5 are published.
GR0 and all unrelated banks/streams are preserved. Bank0 is excluded; the first
unreferenced disabled stage1 bank (starting at1), invalid SMR and unused nonzero
ASID are selected. Any valid SMR whose mask matches SID5 causes refusal.
The ID0 ATOSNS bit is set: no SMMU hardware translation-op test is attempted.
The actual DMA translation proof must come from firmware boot/HFI on the phone.

Firmware matches Linux `a6xx_gmu_fw_load`: 11 zero-size prealloc headers and
4 data blocks, ITCM 0/0x1dc, icache 0x4000/0x10210, DTCM
0x10007f98/0x28 and 0x10007fdc/0x18. Prealloc extents inside ITCM/DTCM/icache/
dummy are checked but do not copy payload. Total data bytes=0x1042c (66604).
No firmware bytes are tracked or copied into tracked paths.

## 4. New report word map

Each page has 128 little-endian u32. Page0 preserves every old word (below),
adding **[2] latest stage entered**, **[3] permanent GMU-start latch**.
Stage8a is encoded as 0x8a in page0[1]/[2].

Page1 (selector 0x101), indices local to that page:
```
[0] 0xa7400002 (report format v2)
[1] snapshot status (1 complete) [2] stage8 result [3] stage9 result
[4] GMU-start/handoff-lock latch
[5] SMR count [6] CB count [7] register page stride [8] CB region offset
[9] pre-stage8 GFSR
[10] chosen SMR slot [11] CB [12] ASID [13:14] table PA low/high
[15] context programmed (configuration issued; not itself success)
[16] last TLBSTATUS [17] firmware bytes [18] data block count
[19] zero-size/prealloc count [20] data bytes [21] bad address [22] bad size
[23] CBAR [24] CBA2R [25] TCR2 [26] TCR [27:28] TTBR0 low/high
[29] MAIR0 [30] SCTLR [31] published SMR [32] published S2CR
[33] table pages used
[34] DTCM firmware version word [35] expected init result
[36] observed FW_INIT_RESULT [37] HFI_CTRL_STATUS [38] HFI_VERSION_INFO
[40:43] PERF_TABLE ACK record
[44:47] BW_TABLE ACK record
[48:51] CORE_FW_START ACK record
[52:55] START ACK record
[56:59] GX_BW_PERF_VOTE ACK record (stage10)
  each: sent header / response header / returned command header / error
[60] stage10 result [61] OOB interrupt sample [62] SPTPRAC status
[63] GX GDSCR [64] RBBM_STATUS [65] RBBM read valid
[84] GMU2HOST_INTR_INFO [85] AO_HOST_INTERRUPT_STATUS [86] AHB_FENCE_STATUS
[87] live global SMMU GFSR [88] chosen CB FSR [89:90] FAR low/high
[91:92] FSYNR0/1
[93:94] command queue read/write [95:96] response queue read/write
[100:109] GPU ARC votes, OFF then 220/295/348/401/475/550/615/680/719 MHz
[110:112] GMU ARC votes, OFF/200/500 MHz
```

Page2 (0x102): `[2*i] SMR(i), [2*i+1] S2CR(i)`, i<count (Fuxi24,
geometry limit32). Additional context controls: `[64+b] ACTLR(b)` and
`[88+b] CONTEXTIDR(b)`, b<CB count. ACTLR is observed and preserved.
Pages3..5 (0x103..105): eight banks per page; bank b starts at
`page=3+b/8, word=16*(b%8)`, with these 16 fields:
```
CBAR, CBA2R, SCTLR, TCR2, TTBR0lo, TTBR0hi, TTBR1lo, TTBR1hi,
TCR, MAIR0, MAIR1, FSR, FARlo, FARhi, FSYNR0, FSYNR1
```

Healthy expectations (predictions for stages8+, not observations):
- 8a: page1[1]=1, [5:8]=24/22/0x1000/0x20000. Review **all** streams and
  enabled banks before the first stage8 run. Existing SID5 match means stop.
- 8: [2]=1, firmware 66844 B/4 blocks/11 prealloc/66604 data B, 13 table pages;
  SMR=0x80000005, S2CR=selected CB, TTBR=0x8f0000000 | ASID<<48,
  TCR etc as above; chosen FSR zero. GMU latch remains zero.
- 9: [3]=1, [4]=1. Init is `0xbabeface` for DTCM version <=0x20010004,
  otherwise `(result & 0x1ff)==0x100`. HFI status bit0=1. Four ACKs must have
  id126/type1, exact returned command headers and error0. Sent headers are
  `0x00103b04 / 0x0020a003 / 0x0030020e / 0x0040010a`.
- 10: [60]=1, vote ACK error0 (`sent=0x0050041e`), OOB sample bit31=1;
  `(SPTPRAC & 0xc3)==0` (Linux GX-off bits6/7 plus transition bits0/1),
  GX GDSCR bit31=1, [65]=1. RBBM_STATUS is evidence only: no exact idle
  value is promised before GPU hw_init. Chosen CB FSR should remain zero.

Results: 0=not run, 1=success. Codes (hex):

| Code | Failure |
|---|---|
| 801 | Unsupported SMMU geometry/capability/client-disabled state |
| 802 | No externally embedded GMU firmware |
| 803 | Malformed/truncated/alignment/unmapped data or prealloc extent |
| 804 | Existing exact or masked SMR matches SID5 |
| 805 | No unused disabled CB/free SMR/unused ASID, or bank became active |
| 806 | Stage8 cold prerequisites not met |
| 807 | Scratch overlaps runtime image or incoming FDT |
| 808 | SMMU TLB sync timeout |
| 809 | SMMU context/stream readback mismatch |
| 80a | Firmware dword readback mismatch |
| 80b | Page-table pool exhausted/overlapping mapping |
| 80c | Chosen CB fault after firmware load |
| 901 / 902 | Firmware init / HFI init timeout or CM3 fault |
| 903 | HFI ACK deadline expired |
| 904 | Corrupt indices, queue full, bad/partial/oversize response |
| 905 | Firmware error packet or CM3 fault |
| 906 | ACK id/type/returned header/sequence mismatch |
| 907 | ACK nonzero error (record holds firmware error) |
| 908 | Missing/malformed cmd-db aux data, uncovered ARC level or missing BW resource |
| 909 | Cold state lost before stage9 start |
| a01 / a02 / a03 | GPU_SET timeout/fault / GX status timeout/fault / GX GDSC still off |
| a04 | Chosen CB fault before GX request |

HFI failures in stage10 use 903..907 in [60]. No destructive recovery is
attempted after failures; reset before changing firmware/tables or retrying.

### Legacy page0 word map (128 × u32, little-endian)
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

## 5. HFI and implementation assumptions

The supplied Linux uses **two** queues (command0, response1), not the three in
the old lane plan. HFI BO: 16 KiB, header at0 (six u32), two 12-u32 queue
headers at24/72 bytes, 4 KiB rings at+4096/+8192. Table `size=120` follows the
Linux implementation's bytes, despite its header comment saying dwords. Rings
have 1024 words, leave one slot empty, align packets to four words and pad
commands with 0xfafafafa. Header size counts unpadded words. Only matching
id126/type1 ACKs with error0 count as success.

The perf table contains OFF plus the nine requested live-DT GPU frequencies in
ascending order (719 MHz represented as 719000 kHz per task; handoff calls top
719.6 MHz). ARC levels = 0/0x34/0x38/0x3c/0x40/0x50/0x80/0x90/0xc0/0xe0.
GMU OFF/200/500 MHz with levels0/0x40/0x80 follows its Linux OPP table. Primary
indices and levels come from gfx.lvl/cx.lvl aux data; secondary from gmxc.lvl
if present, otherwise mx.lvl. Encoding is `primary_level<<16 | secondary_idx<<8
| primary_idx`, including Linux's highest-nonzero secondary fallback.
Every unpopulated GX ACD field is zero, each populated entry uses 0xffffffff,
as Linux does. A740 BW table has one DDR off entry for SH0/MC0/ACV and two CN0
entries off/on; stage10 requests **BW index0**, the only DDR entry, with GPU
perf index1 (220 MHz). It never requests the high GPU frequencies.

Stage9 cold RSC/PDC sequence uses the A740 sequence-memory offset0x154,
hidden data0x80000021 and AOP-resident PDC (enable/start only). No warm-resume
path. GMU chip ID config=0x07020100. It sets FAL veto/TCM retention, boot2,
QTBL, fence range, CM3_CFG=0x4052 and log address as Linux fw_start does.
SYS_BUS/ICACHE/DCACHE config=1. GBIF QSB registers are **not** touched before
GX: Linux sets those during later GPU hw_init. IRQs are masked and polled.
No host GIC setup, zap, or secure-mode calls.

Unverified assumptions/limits: the generic GMU blob matches Fuxi; selected CBs
are writable in this boot's hypervisor environment; the fixed high RAM window
is reachable from the GMU SMMU; the documented GPU-pwrlevels-0 bin is the active
one. No QMP AOSS message to disable ACD is sent; firmware defaults and Linux's
ACD vote fields are used. No special IFPC override is added (Linux A7xx returns
early from power_config). These require device evidence, especially stage9
startup and stage10's live GX status. Offline mocks do not prove power or DMA.

## 6. Build, validation, and coordinator test order

Optional environment input: `FUXI_GMU_FIRMWARE=/absolute/local/path`. The build
writes an `.incbin` assembly file **only in ignored shim/out/fuxi-loader**;
the input remains external. Omitting the variable emits empty symbols, builds
normally, and stage8 reports802 without touching scratch or the SMMU context.
Standalone fuxi-probe builds also remain supported, without embedded firmware.

```sh
FUXI_GMU_FIRMWARE=/hdd2/fuxi-ios/research/firmware/gpu/sheng-firmware/gmu_gen70200.bin \
CLANG=/hdd2/pixelos/prebuilts/clang/host/linux-x86/clang-r547379/bin/clang \
LLD=/hdd2/pixelos/prebuilts/clang/host/linux-x86/clang-r547379/bin/ld.lld \
OBJCOPY=/usr/bin/llvm-objcopy-18 READELF=/usr/bin/llvm-readelf-18 \
NM=/usr/bin/llvm-nm-18 MKBOOTIMG=/hdd2/fuxi-ios/research/mkbootimg/mkbootimg.py \
bash shim/fuxi-loader/build.sh /hdd2/fuxi-ios/shim/out/fuxi-integ1-payload.bin
```

Payload SHA256 `800b9ea17ce18f1872c0533d5ef13f916c490eb7b1524e677fc2555378f00d66`.
Firmware SHA256 is in the facts table. Deliverable (this worktree):
`shim/out/lane-gpu/fuxi-gpu-m2-boot.img` (20,783,104 bytes).
SHA256 `30569542a86b0a1d448a0bb1dc87d3cfde7ad0807f59c09c44b52f1134293916`.
Firmware-enabled ELF: `_gpu_fw_start=0xcc1c`, `_gpu_fw_end=0x1d138`
(66844 bytes), `_bss_start=0x20000`, `_bss_end=0x50000`,
`_payload_start=0x60000`, `_image_end=0x13d0050`. `.bss` is 192 KiB,
not the 32 MiB scratch reservation. Loader builds enforce `-Werror`.

Offline checks completed: firmware-enabled loader, no-firmware loader and
standalone probe all build with **zero warnings**; relocation/Image-size guards
pass. Native C memory model under UBSan and `-Wall -Wextra -Werror` passes with
synthetic firmware and the exact external blob; checks all icache PTEs and holes,
privilege attributes, firmware format bounds/readback, bank/ASID ownership,
masked SID collision, TLB timeout/readback failure, HFI startup order, ACK
errors/sequence/size, ring wrap/full/invalid index, bounded init/ACK timeouts,
and permanent GMU-start latch after init failure. Python decodes all six model
report pages offline. It does **not** execute firmware or emulate power/SMMU.

```sh
python3 tools/test-fuxi-gpu-probe.py
python3 tools/test-fuxi-gpu-probe.py --firmware /absolute/local/gmu_gen70200.bin
python3 tools/fuxi-gpu-probe.py --decode /private/captured-report.bin
```

Coordinator only, serial tests, after committing and cooling the phone:

1. Fresh temporary boot of the milestone2 image; stage7 regression first.
2. Stage **8a** on a fresh boot: `tools/fuxi-gpu-probe.py --stage 8a --raw
   --save research/device/<run>/gpu.bin`. Inspect all SMRs/CBs and confirm SID5
   is unmatched. This is the gate before running stage8.
3. Fresh boot stage **8**; verify context, image block counts, FSR and readbacks.
4. Fresh boot stage **9**; verify init/HFI and all four ACKs. Then coordinator
   tests request0x49 with value0x584e (with MON_ACTIVE set): **must stall**, and
   MON_LAUNCH must remain0. This test never starts XNU. Do not bypass the guard.
5. Only after 8/9 look healthy, fresh boot stage **10**; inspect low vote ACK,
   OOB, GX predicates and RBBM read-valid. No CP command or rendering claim.
6. `--read-reports --raw --save ...` rereads frozen pages without any hardware
   setup. A repeated stage can sample live fault fields but cannot retry setup.
   Preserve raw reports and exact boot/reset results outside git. Any failure:
   stop climbing, capture available pages, let the reset occur, report evidence.

`/hdd2/fuxi-ios/research/run-gpu.py` is read-only for this lane and unchanged.
It mounts the **main checkout** tool into its container. Before using it the
coordinator must integrate the new tracked tool there (otherwise its parser only
accepts1..7), or run this worktree's host tool against the already booted probe.
Pass the explicit milestone2 image path; the runner's old default is milestone1.
The runner itself was not executed here. No CI run or device boot was attempted.

## 7. Remaining work

First concrete action is coordinator stage8a. If stages8–10 succeed on device,
next milestone is GPU user SIDs/context, SQE + hw_init, ring/CP_ME_INIT and one
CP_MEM_WRITE; then an XNU driver and userspace acceleration. Those are outside
this loader-only implementation. Keep Android, all partitions and private
artifacts untouched by these probes.

## Device results (2026-10-20, coordinator)
- 7 PASS (m2). 8a: first m2 boot reset the SoC (sync abort -> fatal -> PSCI reset), not
  reproduced on m2b, which reads the snapshot through `probe_safe_rd32` (0 faults; page1[48..52]).
  Every SMR invalid, S2CR 0x20000, CBAR 0, no bank enabled.
- 8 PASS (slot0 -> CB1, ASID1, 13 tables, fw read back).
- 9: m2b 0x906 - the gen70200 GMU ACK header echoes the command id (0x00010304), not 126.
  m2c accepts either (Linux checks only ret_header seq). m2c 9 PASS, all ACK errors 0.
- 0x49 after GMU start: STALL. 10 PASS: GX_GDSCR 0xf8222800, RBBM_STATUS 0x1.

