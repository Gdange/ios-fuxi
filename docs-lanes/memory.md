# Fuxi RAM layout (milestone 7)

The coordinator's saved live FDT advertises `0x8c0000000 + 0x140000000`
(5 GiB), with no reserve-map entries or reserved-memory overlap in this bank.
`mkadt-fuxi.py` validates the **whole bank**, including scratch, against both
reservation mechanisms before producing an ADT. XNU manages one contiguous range.

| Use | Start | End (exclusive) |
| --- | --- | --- |
| XNU, 5088 MiB | `0x8c0000000` | `0x9fe000000` |
| GPU, 32 MiB | `0x9fe000000` | `0xa00000000` |
| PAS metadata (inside GPU) | `0x9ff600000` | `0x9ff602000` |
| Cold handoff (inside GPU) | `0x9ff800000` | `0x9ff804000` |
| Three firmware slots (inside GPU) | `0x9ff804000` | `0x9ffb04000` |

The zap ELF's authenticated load/mem_setup region remains `0x9b09a000 + 0x1000`,
outside this DRAM bank. It is firmware-defined and must not follow scratch.
GMU/SMMU/ring/self-test addresses are relative to the relocated `G_MEM`.
Loader staging is allowed only for the explicit full or original 1 GiB payload
sizes; its existing segment, ADT, boot-args and loader/FDT overlap checks apply.
Both boot-args `memSize` and `memSizeActual` take the ADT's 64-bit dram-size.
`fuxi-gpu-memory` and actual `gPhysBase/gPhysSize` are checked again inside XNU.

Source facts: patch 0082 selects `MASTER.arm64.MacOSX`, open ARM pmap/assembly,
16 KiB pages and generic SWEET hardware (no KTRR/CTRR). `MakeInc.def` therefore
sets `ARM_LARGE_MEMORY=1`, with the `0xfffffe...` kernel link address. In
`osfmk/arm/pmap/pmap.h`, this selects 34 bootstrap pages, documented for 1 TiB
with 16 KiB pages. L1 covers 64 GiB, L2 blocks cover 32 MiB: the 5 GiB physical
range and initial kernel mapping need two L1 roots plus at most two L2 pages
per root, comfortably below 34. `arm_vm_init.c` derives the separate physmap
size from the 64-bit boot memory size plus 1 GiB slide and alignment padding;
its 576 GiB kernel VA range and unsupported-memory check do not cap us at 4 GiB.
`max_mem`, `mem_size`, `gPhysSize` and ARM64 `vm_size_t` are 64-bit. There is no
new `maxmem` boot argument. GPU translations accept PAs below 2^36, including
the entire selected bank. No 1 GiB constant in `mkadt.py` is inherited: its
highmem globals are explicitly replaced by the Fuxi builder. The old 1 GiB
loader RAM **sampling probe** remains a bounded diagnostic, not a VM limit.

These facts support all 5 GiB minus the required 32 MiB scratch; they do not
prove upper-bank hardware coherency or runtime stability. No device test is
claimed. XNU allocation metadata overhead will consume part of the 5088 MiB.

Use `--ram-layout 1g` to reproduce the original 1 GiB XNU window. With
`--gpu-reserve` it uses the new top-of-bank scratch, so a newly built GPU kernel
also works with this reduced window. Old 768 MiB images require their matching
old loader/kernel. `--template --gpu-reserve` preserves identity, CPUs and root
extents while changing memory properties only. The default is full RAM.

USB read request 0x47 still encodes a 32-bit offset (low 4 GiB). New read-only
request 0x58 encodes the same fields plus 4 GiB. Both reject reads beyond the
5088 MiB XNU window. The host also bounds reads by its payload's memory size.
