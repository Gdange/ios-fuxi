# WLAN M6i: PCIe0 interconnect votes

Source-only experiment for the PBL BHI read failure. This milestone does not
establish that bandwidth voting fixes ERRCODE `0xef0e0003`. No kernel build,
CI dispatch, phone access or firmware change was performed.

## Source derivation

The reference is Linux **v6.18**:

* [sm8550.dtsi](https://github.com/torvalds/linux/blob/v6.18/arch/arm64/boot/dts/qcom/sm8550.dtsi):
  PCIe0 requests `pcie-mem` from `pcie_noc:MASTER_PCIE_0` to
  `mc_virt:SLAVE_EBI1`, tagged ALWAYS; `cpu-pcie` from
  `gem_noc:MASTER_APPSS_PROC` to `cnoc_main:SLAVE_PCIE_0`, tagged ACTIVE_ONLY.
* [pcie-qcom.c](https://github.com/torvalds/linux/blob/v6.18/drivers/pci/controller/dwc/pcie-qcom.c):
  `qcom_pcie_icc_init` requests average/peak **0/250000 kB/s** for memory
  before enabling interconnect clocks, and **0/1 kB/s** for CPU register
  access. `qcom_pcie_icc_opp_update` uses negotiated width times the encoded
  link payload rate: Gen1 250000, Gen2 500000, Gen3 984500 kB/s per lane.
  The previously observed Gen2 x2 therefore requests **0/1000000 kB/s**.
* [sm8550.c](https://github.com/torvalds/linux/blob/v6.18/drivers/interconnect/qcom/sm8550.c):
  the memory route is `xm_pcie3_0 -> qns_pcie_mem_noc -> qnm_pcie ->
  qns_llcc -> llcc_mc -> ebi`. The CPU route is `chm_apps -> qns_pcie ->
  qnm_gemnoc_pcie -> xs_pcie_0`. Its shorter route uses the GEM-to-PCIe
  bridge, rather than traversing the configuration-NOC peripheral fanout.

| BCM | Provider / relevant node | Memory path | CPU path | Rule |
| --- | --- | --- | --- | --- |
| SN7 | pcie_noc / qns_pcie_mem_noc | yes | no | bandwidth, buswidth 16, channels 1 |
| SH1 | gem_noc / qnm_pcie, chm_apps, qns_pcie | yes | yes | enable mask 1, x/y = 0/1 |
| SH0 | gem_noc / qns_llcc | yes | no | bandwidth, buswidth 16, channels 4 |
| MC0 | mc_virt / ebi | yes | no | bandwidth, buswidth 4, channels 4 |
| ACV | mc_virt / ebi | yes | no | enable mask 8, x/y = 0/8 |
| CN0 | cnoc_main / qnm_gemnoc_pcie, xs_pcie_0 | no | yes | enable mask 1 plus keepalive, ACTIVE x/y = 1/1 |

Neither route crosses **aggre2**. Its CE0 BCM belongs to the crypto master;
adding it would not supply the PCIe memory path. `llcc_mc` is on the route
but has no BCM of its own. MC0 and SH0 have keepalive fallback 1/1 only if
both calculated votes are zero; this experiment always supplies memory peak.

## CNSS2 / KIWI audit

The downstream PCIe host's `icc_path` in the supplied DT excerpt is a single
PCIe0-to-EBI path, using the downstream two-cell binding. It is not evidence
for a second CPU path in that DT. M6i uses the mainline paths above as the
explicit source recipe, including the mainline CPU access vote.

Public downstream references are pinned separately:

* [KIWI chip_cfg@1 in kalama-cnss.dtsi, 43b5ada5654ed84c25f74789bc8f6326b7bacd20](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-devicetrees/blob/43b5ada5654ed84c25f74789bc8f6326b7bacd20/qcom/wlan/kalama-cnss.dtsi):
  device ID 0x1107 / RC0; two ICC paths and nine bandwidth rows.
* [CNSS2 main.c, 92a9fde479d3262da080cfd28bab0775b0da500a](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/main.c):
  `cnss_register_bus_scale` reads `qcom,icc-path-count`,
  `qcom,bus-bw-cfg-count`, names and `qcom,bus-bw-cfg` into per-path tables.
* [CNSS2 pci.c at the same revision](https://github.com/LineageOS/android_kernel_xiaomi_sm8550-modules/blob/92a9fde479d3262da080cfd28bab0775b0da500a/qcom/opensource/wlan/platform/cnss2/pci.c):
  `cnss_setup_bus_bandwidth` passes a selected row directly to `icc_set_bw`.
  The KIWI powerup path uses `cnss_qca6290_powerup`, resumes PCIe and calls
  `cnss_pci_start_mhi`; INIT/POWER_ON invoke MHI prepare/sync-power-up.
  **These functions do not request a nonzero CNSS ICC row for firmware
  download.** Internal bandwidth calls remove votes on shutdown/suspend or
  restore the saved row on resume. `cnss_mhi_bw_scale` changes PCIe link
  speed/width through the host driver; it does not select a bus-table row.

Thus no additional automatic CNSS firmware-download BCM can be derived from
this pinned implementation. A DT table alone does not establish a call to
select HIGH or VERY_HIGH. M6i defaults to row 0; the optional
`fuxiwlaniccbw` argument makes every KIWI row available for an explicit
bandwidth experiment before BHI. Both paths use the same six BCMs already
listed, with no additional BCM names.

| Row | DT description | Average, both paths | pcie_to_memnoc peak | memnoc_to_ddr peak |
| --- | --- | ---: | ---: | ---: |
| 0 | none (default) | 0 | 0 | 0 |
| 1 | idle | 2250 | 1600000 | 2188800 |
| 2 | low | 7500 | 1600000 | 2188800 |
| 3 | medium | 30000 | 1600000 | 2188800 |
| 4 | high | 100000 | 1600000 | 2188800 |
| 5 | very high | 175000 | 6448000 | 6220800 |
| 6 | ultra high | 312500 | 6448000 | 8368000 |
| 7 | super high | 587500 | 8528000 | 12800000 |
| 8 | low latency | 7500 | 3200000 | 2188800 |

All entries are kB/s. The first path ends at `SLAVE_ANOC_PCIE_GEM_NOC`;
the second starts at `MASTER_ANOC_PCIE_GEM_NOC` and ends at `SLAVE_EBI1`.
Their disjoint node sets mean their averages are not added together at one
node. The mainline memory client has zero average. Each segment's aggregate
peak is the maximum of link peak and that segment's CNSS peak.

## Runtime lookup, calculation and TCS encoding

[cmd-db.c](https://github.com/torvalds/linux/blob/v6.18/drivers/soc/qcom/cmd-db.c)
and [icc-rpmh.c](https://github.com/torvalds/linux/blob/v6.18/drivers/interconnect/qcom/icc-rpmh.c)
define the database layout and BCM aux data. M6i uses the WLAN regulator
code's existing cmd-db mapping and byte accessors, then validates header
version/magic, standalone flag, slave and entry extents, exact zero-padded
resource names, unique matches, BCM slave ID 5, aligned command addresses,
aux length/extent, nonzero unit/width and VCD 0..9. Duplicate addresses
between required BCM names are rejected. No raw BCM MMIO access occurs.
All six lookups must succeed before sending any vote. Addresses, units,
widths, VCDs and aux versions are logged at runtime, never guessed.

For each bandwidth BCM, let **U** and **W** be its own cmd-db aux unit and
width, **B** its node buswidth, **C** its channel count, and **A/P** the
aggregate average/peak in kB/s. Define `d(n,b)` as integer floor `n/b`,
except a nonzero `n < b` returns 1. Linux's two normalization stages are:

```
x = min(16383, d(d(A * W, B * C) * 1000, U))
y = min(16383, d(d(P * W, B)     * 1000, U))
```

The factor 1000 is the default vote_scale in `qcom_icc_bcm_init`.
Peak normalization deliberately does not divide by channel count.
The calculation uses 64-bit intermediates and saturates each 14-bit vote.
For default Gen2 x2, A=0 and P=1000000 on SN7, SH0 and MC0:

| BCM | Default ACTIVE x/y before encoding |
| --- | --- |
| SN7 | 0 / min(16383, d(d(1000000 * W, 16) * 1000, U)) |
| SH0 | 0 / min(16383, d(d(1000000 * W, 16) * 1000, U)) |
| MC0 | 0 / min(16383, d(d(1000000 * W, 4) * 1000, U)) |
| SH1 | 0 / 1 |
| ACV | 0 / 8 |
| CN0 | 1 / 1 |

SN7 and SH0 can have different metadata despite identical formulas. Exact
numeric bandwidth vote fields cannot be stated without the runtime aux data;
host tests use invented metadata, not extracted device bytes.

[bcm-voter.c](https://github.com/torvalds/linux/blob/v6.18/drivers/interconnect/qcom/bcm-voter.c)
and [tcs.h](https://github.com/torvalds/linux/blob/v6.18/include/soc/qcom/tcs.h)
place commit in bit30, valid in bit29, x in bits27:14 and y in bits13:0:

```
data = (commit << 30) | ((x != 0 || y != 0) << 29) | (x << 14) | y
```

The enable/mask command words before commit are SH1 `0x20000001`,
ACV `0x20000008`, CN0 `0x20004001`; add `0x40000000` only if last in VCD.
The command list is stably sorted by ascending runtime VCD. Only the final
command of each domain gets commit and a response/wait request. All six
commands share one ACTIVE TCS payload, so a domain cannot be split.

The existing WLAN `rh_send` regulator transport now wraps the same bounded
batch sender. [rpmh-rsc.c](https://github.com/torvalds/linux/blob/v6.18/drivers/soc/qcom/rpmh-rsc.c)
defines the v3 TCS stride of 672 bytes, **command stride of 24 bytes**, message
length 8, WRITE bit16 and response-request bit8. The sender checks revision,
DRV2 TCS count and commands-per-TCS, refuses occupied TCS0, masks only its IRQ
source, checks programmed command readback, and requires a fresh completion
IRQ status, idle TCS and issued status for every command plus completion for
every response command. Polling is bounded to 100000 iterations with 1 us
delays. An uncertain transfer leaves TCS0 quarantined and stops bring-up;
there is no reset or retry. Successful cleanup restores its prior IRQ-enable
bit, preserves other IRQ sources/TCS slots and does not remove BCM votes.

## Ordering, arguments and expected logs

The main `fuxiwlan` gate remains default off. With WLAN enabled:

* `fuxiwlanicc=1` is the default. Before step 2 (and therefore before fabric
  clocks, PERST release or LTSSM), send the 0/250000 and 0/1 requests.
* After successful step 11, before step 12 endpoint setup, re-read the link
  and send its negotiated memory peak. Gen1..3 x1..2 are supported;
  missing DLLLA, unknown speed/width or an access fault stops the run.
* The selected `fuxiwlaniccbw=0..8` row (default 0) is included in this second
  batch. Both batches precede step 21's BHI doorbell. Invalid arguments fail
  at step 2. `fuxiwlanicc=0` bypasses lookups and votes for an A/B run.
* Votes remain on successful completion, partial runs, BHI failure and DMA
  cleanup. No teardown zero vote is sent. Existing `fuxiwlanoldiova`,
  `fuxiwlanclk` and `fuxiwlaniboff` behavior remains available.

Expected line shapes (angle brackets denote runtime fields):

```
FUXI WLAN: M6i ICC <bcm> cmd-db addr <addr> unit <U> width <W> VCD <vcd> aux-version <version> lookup 0
FUXI WLAN: M6i ICC pretrain pcie-mem 0/250000 cpu-pcie 0/1 CNSS row 0 path1 0/0 path2 0/0 kBps
FUXI WLAN: M6i ICC link/download pcie-mem 0/1000000 cpu-pcie 0/1 CNSS row 0 path1 0/0 path2 0/0 kBps
FUXI WLAN: M6i ICC <bcm> addr <addr> vote <x>/<y> data <word> VCD <vcd> commit <0-or-1> ack yes error 0
```

There should be six lookup lines and six acknowledged command lines per
batch. The M6h host snapshot now reports `XNU_batches=2` after both successful
batches, instead of claiming no XNU votes. Failure reports `ack unconfirmed`
and stops at step 2 or 12, without proceeding to BHI. An ACK establishes
transport/BCM command completion; it does not prove the PBL read succeeded.

## Validation and limits

`python3 tools/test-fuxi-wlan-m6i.py` compiles the production parser,
calculation, encoding, transport and `fw_run` hooks with ASan/UBSan. Synthetic
fixtures cover lookup failures/extents/aliases, runtime metadata conversion,
small-value floors, saturation, masks/keepalive, all nine CNSS rows, stable
VCD grouping, per-domain commit/wait/response bits, TCS geometry and register
readback, missing command completion, stale status, busy/failed cleanup,
MMIO faults, quarantine and no-BHI ordering on either batch failure.
`python3 tools/test-fuxi-wlan-m6.py` retains DMA/BHI containment tests and
checks standalone source identity against integration patch 0099.

The existing `tools/ci/build.sh kernel-wlan6g` target runs both suites;
its target, artifact names, builder integration and kernel-input cmdlines
are unchanged. M6i is default-enabled inside the existing WLAN gate, so
current CI cmdlines require no edit. No CI workflow was dispatched here.

Risks: these direct commands replace this DRV's inherited vote for the same
resources; there is no common XNU ICC client aggregator or way here to
recover that inherited bandwidth. Other DRVs remain hardware-aggregated,
but their votes are unknown. Keepalive/mask semantics are mainline SM8550;
the downstream platform and firmware may have additional QoS requirements.
ACTIVE votes are retained for this running session; this is not a port of
Linux's sleep/wake TCS cache or RPMh system-suspend integration. Entering
RPMh sleep could reapply inherited sleep/wake values. Higher optional CNSS
rows can increase power/temperature; 14-bit saturation is visible in logs.
Runtime cmd-db and hardware behavior remain unverified, and the PBL failure
may have another cause.
