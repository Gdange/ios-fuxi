/* Lane-wlan (milestone 1): read-only WCN7850 / PCIe0 / BT-UART snapshot on
 * request 0x5c, value 0x574c ('WL'), index 1. Included from probe.c after
 * gpu.c (uses probe_safe_rd32). No register writes of any kind.
 *
 * Sources (see docs-lanes/wlan-bt.md):
 *  GCC offsets      research/gcc-kalama.c (base 0x100000)
 *  TCSR clkref      tcsrcc-sm8550.c tcsr_pcie_0_clkref_en 0x15100 (base 0x1fc0000)
 *  PCIe0 regs       live DT qcom,pcie@1c00000 (parf 0x1c00000, phy 0x1c06000,
 *                   dm_core 0x60000000), mainline pcie-qcom.c PARF offsets
 *  GDSCs            live DT qcom,gdsc@16b004 / @16c000, collapse vote 0x152020
 *  BT UART          live DT qcom,qup_uart@898000 (QUP2 SE6), wrapper 0x8c0000
 *  GPIOs            cnss: wlan-en 80, bt-en 81, sw-ctrl 82, wlan-sw-ctrl 83,
 *                   uart 76-79; pcie0 perst 94, clkreq 95, wake 96
 *  cmd-db           reserved aop_cmd_db_region 0x81c60000 + 0x20000
 *
 * Gating (touch-lane lesson: an unclocked peripheral read hangs the NoC and
 * resets the SoC; probe_safe_rd32 only recovers synchronous aborts):
 *  PARF   read only when pcie_0 GDSC reports on and cfg_ahb CBCR is running
 *  PHY    additionally phy GDSC on and aux CBCR running
 *  DBI    additionally slv_axi CBCR running
 *  SE6    read only when wrap2 m_ahb CBCR is running
 */
#define WL_GCC    0x00100000UL
#define WL_TCSR   0x01fc0000UL
#define WL_TLMM   TS_TLMM
#define WL_PARF   0x01c00000ULL
#define WL_PHY    0x01c06000ULL
#define WL_DBI    0x60000000ULL
#define WL_QUP2   0x008c0000ULL
#define WL_SE6    0x00898000ULL
#define WL_CMDDB  0x81c60000UL
#define WL_CMDDB_BYTES 0x20000u
#define WL_MAGIC  0x574c0001u

static uint32_t wlan_report[128];

static uint32_t wl_srd(uint64_t pa, unsigned *faults)
{
 uint32_t f = 0, v = probe_safe_rd32(pa, &f);
 if (f) (*faults)++;
 return v;
}
static int wl_on(uint32_t cbcr) { return !(cbcr & 0x80000000u); }
static uint8_t wl_cdb8(uint32_t off) { return (uint8_t)(usb_rd(WL_CMDDB, off & ~3u) >> ((off & 3u) * 8)); }
static uint32_t wl_cdb16(uint32_t off) { return wl_cdb8(off) | (uint32_t)wl_cdb8(off + 1) << 8; }

/* cmd-db resource address by 8-byte name; 0 when absent or malformed. */
static uint32_t wl_cmddb_addr(const char *name)
{
 for (unsigned s = 0; s < 8; s++) {
  uint32_t row = 8 + s * 16;
  if (!wl_cdb16(row)) break;
  uint32_t at = 144 + wl_cdb16(row + 2), n = wl_cdb16(row + 6);
  if (at > WL_CMDDB_BYTES || n > (WL_CMDDB_BYTES - at) / 24) return 0;
  for (uint32_t i = 0; i < n; i++) {
   uint32_t e = at + i * 24; unsigned j;
   for (j = 0; j < 8 && name[j]; j++) if (wl_cdb8(e + j) != (uint8_t)name[j]) break;
   if (j < 8 && (name[j] || wl_cdb8(e + j))) continue;
   return wl_cdb16(e + 16) | wl_cdb16(e + 18) << 16;
  }
 }
 return 0;
}

static void wlan_snapshot(void)
{
 uint32_t *r = wlan_report;
 unsigned faults = 0, n;
 memset(r, 0, sizeof(wlan_report));
 r[0] = WL_MAGIC; r[2] = 1;
 /* GCC branch votes and GDSC collapse vote (always-on GCC block). */
 r[4] = usb_rd(WL_GCC, 0x52000); r[5] = usb_rd(WL_GCC, 0x52008);
 r[6] = usb_rd(WL_GCC, 0x52010); r[7] = usb_rd(WL_GCC, 0x52020);
 static const uint32_t pcie_cbcr[] = {0x6b020, 0x6b02c, 0x6b038, 0x6b03c, 0x6b048,
                                      0x6b01c, 0x6b054, 0x1003c, 0x1004c, 0x10030};
 for (n = 0; n < 10; n++) r[8 + n] = usb_rd(WL_GCC, pcie_cbcr[n]);
 r[19] = usb_rd(WL_GCC, 0x6b004); r[20] = usb_rd(WL_GCC, 0x6b008);
 r[21] = usb_rd(WL_GCC, 0x6c000); r[22] = usb_rd(WL_GCC, 0x6c004);
 static const uint32_t bcr[] = {0x6b000, 0x6c01c, 0x6c014, 0x6c020, 0x6c028};
 for (n = 0; n < 5; n++) r[23 + n] = usb_rd(WL_GCC, bcr[n]);
 r[28] = usb_rd(WL_GCC, 0x6b070);
 r[29] = usb_rd(WL_GCC, 0x6b074); r[30] = usb_rd(WL_GCC, 0x6b078);
 r[31] = usb_rd(WL_GCC, 0x6b058); r[32] = usb_rd(WL_GCC, 0x6b05c);
 r[33] = usb_rd(WL_TCSR, 0x15100);
 r[34] = usb_rd(WL_GCC, 0x233cc); r[35] = usb_rd(WL_GCC, 0x233d0);
 r[36] = usb_rd(WL_GCC, 0x233d4); r[37] = usb_rd(WL_GCC, 0x23004);
 r[38] = usb_rd(WL_GCC, 0x1e754);
 /* SE6 RCG: CMD, CFG, DFSR, 8 DFS perf levels with M and N. */
 r[39] = usb_rd(WL_GCC, 0x1e760); r[40] = usb_rd(WL_GCC, 0x1e764);
 r[41] = usb_rd(WL_GCC, 0x1e774);
 for (n = 0; n < 8; n++) {
  r[42 + n] = usb_rd(WL_GCC, 0x1e77c + 4 * n);
  r[50 + n] = usb_rd(WL_GCC, 0x1e7bc + 4 * n);
  r[58 + n] = usb_rd(WL_GCC, 0x1e7fc + 4 * n);
 }
 /* TLMM CFG and IN_OUT: gpio76..83, then 94..96. */
 for (n = 0; n < 8; n++) {
  r[66 + n] = usb_rd(WL_TLMM + 0x1000u * (76 + n), 0);
  r[74 + n] = usb_rd(WL_TLMM + 0x1000u * (76 + n), 4);
 }
 for (n = 0; n < 3; n++) {
  r[82 + n] = usb_rd(WL_TLMM + 0x1000u * (94 + n), 0);
  r[85 + n] = usb_rd(WL_TLMM + 0x1000u * (94 + n), 4);
 }
 r[88] = usb_rd(RH_DRV_PA, 0); r[89] = usb_rd(RH_DRV_PA, 0xc);
 /* cmd-db: header magic, then the RPMh addresses the later stages would vote. */
 r[90] = usb_rd(WL_CMDDB, 4);
 if (r[90] == 0x0c0330db) {
  static const char names[10][8] = {"ldob15", "smpe4", "smpg2", "smpg4", "smpg5",
                                    "smpg6", "ldoe1", "ldoe3", "clka1", "vrm.wlan"};
  for (n = 0; n < 10; n++) {
   char nm[9]; memcpy(nm, names[n], 8); nm[8] = 0;
   r[91 + n] = wl_cmddb_addr(nm);
  }
 }
 /* Gated PCIe0 reads. */
 int core_on = (r[19] & 0x80000000u) && wl_on(r[10]);
 if (core_on) {
  r[101] |= 1;
  r[102] = wl_srd(WL_PARF + 0x1b0, &faults);  /* LTSSM */
  r[103] = wl_srd(WL_PARF + 0x40, &faults);   /* PHY_CTRL */
  r[104] = wl_srd(WL_PARF + 0x1000, &faults); /* DEVICE_TYPE */
  r[105] = wl_srd(WL_PARF + 0x224, &faults);  /* INT_ALL_STATUS */
  r[106] = wl_srd(WL_PARF + 0x350, &faults);  /* DBI_BASE_ADDR_V2 */
  r[107] = wl_srd(WL_PARF + 0x634, &faults);  /* ATU_BASE_ADDR */
  if ((r[21] & 0x80000000u) && wl_on(r[11])) {
   r[101] |= 2;
   r[108] = wl_srd(WL_PHY + 0x214, &faults);  /* PCS status (bit6 PHYSTATUS) */
   r[109] = wl_srd(WL_PHY + 0x240, &faults);  /* power down control */
  }
  if (wl_on(r[8])) {
   r[101] |= 4;
   r[110] = wl_srd(WL_DBI + 0x0, &faults);    /* RC vendor/device */
   r[111] = wl_srd(WL_DBI + 0x18, &faults);   /* bus numbers */
   r[112] = wl_srd(WL_DBI + 0x80, &faults);   /* PCIe cap LNKCTL/LNKSTA */
  }
 }
 /* Gated QUP2 SE6 reads. */
 if (wl_on(r[34])) {
  r[101] |= 8;
  r[113] = wl_srd(WL_QUP2 + 0x4, &faults);    /* QUP HW version */
  r[114] = wl_srd(WL_SE6 + 0x68, &faults);    /* FW_REVISION_RO (proto 15:8, UART 2) */
  r[115] = wl_srd(WL_SE6 + 0x40, &faults);    /* GENI_STATUS */
  r[116] = wl_srd(WL_SE6 + 0x48, &faults);    /* M_CLK_CFG */
  r[117] = wl_srd(WL_SE6 + 0x7c, &faults);    /* CLK_SEL */
  r[118] = wl_srd(WL_SE6 + 0x908, &faults);   /* IOS (bit1 low = CTS asserted) */
  r[119] = wl_srd(WL_SE6 + 0xe24, &faults);   /* HW_PARAM_0 */
  r[120] = wl_srd(WL_SE6 + 0x258, &faults);   /* DMA_MODE_EN */
 }
 r[124] = faults;
 r[125] = probe_fault_esr;
 r[3] = 1;   /* snapshot complete */
}
