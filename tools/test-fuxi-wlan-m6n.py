#!/usr/bin/env python3
"""Compile the actual M6n loader with an AOP peer; no hardware or firmware."""
from pathlib import Path
import importlib.util
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('m6', ROOT / 'tools/test-fuxi-wlan-m6.py')
m6 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m6)

PEER = r'''
static bool aop_reply=true;
static unsigned packets, stop_packet;
static uint64_t aop_write_fault;
static char packets_seen[13][64];
static void wl_pwr(uint64_t pa, uint32_t value) {
    if(fw_faults && (pa==0x40800c || (pa>=0xc300000 && pa<0xc300400))) return;
    if(pa==aop_write_fault) { fw_faults++; return; }
    raw_wl_pwr(pa,value);
    if(pa!=0x40800c || !aop_reply) return;
    assert(value==0);
    if(wl_prd(0xc300024)==0xffff) { setreg(0xc300028,0xffff); setreg(0xc30000c,0xffff); }
    if(wl_prd(0xc30002c)==0xffff) setreg(0xc300014,0xffff);
    if(wl_prd(0xc300018)==0xffff) setreg(0xc300030,0xffff);
    unsigned off=wl_prd(0xc300038);
    if(!wl_prd(0xc300000+off)) return;
    assert(wl_prd(0xc300000+off)==64 && packets<13);
    for(unsigned w=0;w<16;w++) {
        uint32_t v=wl_prd(0xc300004+off+w*4);
        for(unsigned b=0;b<4;b++) packets_seen[packets][w*4+b]=v>>(b*8);
    }
    packets++;
    if(stop_packet!=packets) setreg(0xc300000+off,0);
}
'''
TESTS = r'''
static void setup(bool up) {
    init(); aop_reply=true; packets=stop_packet=0; aop_write_fault=0;
    memset(packets_seen,0,sizeof(packets_seen));
    cnss_arg="pdc"; wl_probe_configure();
    setreg(WL_AOSS_QMP,0x4d41494c); setreg(WL_AOSS_QMP+4,1);
    setreg(WL_AOSS_QMP+0x38,0x80); setreg(WL_AOSS_QMP+0x34,0x80);
    setreg(WL_AOSS_QMP+0xc,0xffff); setreg(WL_AOSS_QMP+0x14,up?0xffff:0xffff0000);
    setreg(WL_AOSS_QMP+0x28,up?0xffff:0xffff0000);
    setreg(WL_AOSS_QMP+0x30,up?0xffff:0xffff0000);
}
static unsigned countwrite(uint64_t pa) {
    unsigned n=0; for(unsigned i=0;i<ne;i++) n+=events[i].kind=='W' && events[i].pa==pa; return n;
}
int main(void) {
    init(); track_reads=true; assert(!wl_cnss_pdc() && !ne && !nreads && !wl_pdc_attempted);
    cnss_arg="off"; wl_probe_configure(); assert(!wl_cnss_pdc() && !ne && !nreads);
    const char *bad[]={"PDC","pdc,full","pdc-extra-long-suffix"};
    for(unsigned i=0;i<3;i++) {
        init(); cnss_arg=bad[i]; wl_probe_configure();
        assert(wl_variant_invalid && wl5_step(17)==0x6c && !ne);
    }
    setup(true); assert(wl5_step(17)==0x6e && !allocations && !ne);
    for(unsigned up=0;up<2;up++) {
        setup(up); assert(!wl_cnss_pdc() && wl_pdc_acks==13 && packets==13);
        assert(countwrite(WL_AOSS_IPCC+0xc)==(up?13:16));
        const char *expected[]={
            "{class: wlan_pdc, ss: rf, res: s4e.v, upval: 916}",
            "{class: wlan_pdc, ss: rf, res: s4e.v, dwnval: 612}",
            "{class: wlan_pdc, ss: rf, res: s4g.v, upval: 1316}",
            "{class: wlan_pdc, ss: rf, res: s4g.v, dwnval: 944}",
            "{class: wlan_pdc, ss: rf, res: s6g.v, upval: 1864}",
            "{class: wlan_pdc, ss: rf, res: s6g.v, dwnval: 1820}",
            "{class: wlan_pdc, ss: rf, res: s2g.m, enable: 0}",
            "{class: wlan_pdc, ss: rf, res: s2g.v, enable: 0}",
            "{class: wlan_pdc, ss: bb, res: pdc, enable: 1}",
            "{class: wlan_pdc, ss: bb, res: s2g.v, upval: 976}",
            "{class: wlan_pdc, ss: bb, res: s2g.v, dwnval: 800}",
            "{class: wlan_pdc, ss: bb, res: s5g.v, upval: 940}",
            "{class: wlan_pdc, ss: bb, res: s5g.v, dwnval: 420}"};
        for(unsigned i=0;i<13;i++) {
            assert(!strcmp(packets_seen[i],expected[i]));
            for(unsigned b=strlen(expected[i]);b<64;b++) assert(!packets_seen[i][b]);
        }
        for(unsigned i=0;i<ne;i++) assert(events[i].pa==WL_AOSS_IPCC+0xc ||
            (events[i].pa>=WL_AOSS_QMP && events[i].pa<WL_AOSS_QMP+0xc4));
        unsigned n=ne; assert(wl_cnss_pdc() && ne==n); /* no second policy submission */
        for(unsigned s=17;s<=21;s++) assert(!wl5_step(s));
        wl_stop22=true; /* explicitly exercise the retained BHI diagnostic */
        assert(wl5_step(22)==0x6c && countwrite(WL_BAR0+wl_bhie+0x3c)==1); /* clear only */
        assert(!wl_mrd(wl_bhie+0x3c)); /* no later AMSS doorbell */
        assert(countwrite(WL_BAR0+wl_bhi+0x18)==1);
        assert(strstr(logbuf,"M6n result cnss=pdc PDC consumed 13/13"));
        assert(wl5_step(21)==0x6b && countwrite(WL_BAR0+wl_bhi+0x18)==1);
        wl5_abort(22); assert(frees==4);
    }
    /* Malformed descriptors cannot cause arbitrary MMIO writes. */
    const unsigned offsets[]={0,0x3c,0x81,0x400,0xfffffffc};
    for(unsigned i=0;i<5;i++) {
        setup(true); setreg(WL_AOSS_QMP+0x38,offsets[i]);
        assert(wl_cnss_pdc()==3 && !ne && !packets);
    }
    const unsigned sizes[]={0,64,0x384,~0u};
    for(unsigned i=0;i<4;i++) {
        setup(true); setreg(WL_AOSS_QMP+0x34,sizes[i]);
        assert(wl_cnss_pdc()==3 && !ne);
    }
    setup(true); setreg(WL_AOSS_QMP+0x80,64);
    assert(wl_cnss_pdc()==3 && !ne && wl_prd(WL_AOSS_QMP+0x80)==64);
    for(unsigned fault=0;fault<3;fault++) {
        setup(true); read_fault_pa=WL_AOSS_QMP+(fault==0?0:fault==1?0x38:0x80);
        assert(wl_cnss_pdc() && fw_faults && !ne && !packets);
    }
    setup(true); setreg(WL_AOSS_QMP+4,2); assert(wl_cnss_pdc()==2 && !ne);
    setup(false); setreg(WL_AOSS_QMP+0xc,0); assert(!wl_cnss_pdc() && packets==13);
    setup(false); setreg(WL_AOSS_QMP+0xc,1); assert(wl_cnss_pdc()==4 && !ne);
    setup(true); aop_write_fault=WL_AOSS_QMP+0x84;
    assert(wl_cnss_pdc()==5 && fw_faults && !packets && !countwrite(WL_AOSS_QMP+0x80));
    setup(true); aop_write_fault=WL_AOSS_IPCC+0xc;
    assert(wl_cnss_pdc()==6 && fw_faults && !packets && wl_prd(WL_AOSS_QMP+0x80)==64);
    setup(false); aop_reply=false;
    assert(wl_cnss_pdc()==4 && sleeps==1000 && !packets && !wl_pdc_acks);
    unsigned n=ne; assert(wl_cnss_pdc() && ne==n);
    setup(true); stop_packet=3;
    assert(wl_cnss_pdc()==6 && sleeps==1000 && packets==3 && wl_pdc_acks==2);
    assert(wl_prd(WL_AOSS_QMP+0x80)==64); /* don't erase outstanding data on timeout */
    assert(wl5_step(17)==0x6e && !allocations);
    n=ne; assert(wl_cnss_pdc() && ne==n);
    /* DMA containment failure retains buffers, even after PDC succeeded. */
    setup(true); assert(!wl_cnss_pdc());
    for(unsigned s=17;s<=21;s++) assert(!wl5_step(s));
    fail_bm=fail_perst=true; wl5_abort(22); assert(!frees && wl_image.va);
    fail_bm=fail_perst=false; wl5_abort(22); assert(frees==4);
    puts("M6n AOP policy, descriptor/peer faults, timeouts, one-shot BHI and containment passed");
    return 0;
}
'''


def main():
    driver = (ROOT / 'xnu/fuxi-wlan/fuxi_wlan.c').read_text()
    patch = (ROOT / 'xnu/patches/0099-fuxi-wlan.patch').read_text()
    added = patch.split('+++ b/bsd/dev/fuxi_wlan.c\n', 1)[1].split('\n--- a/', 1)[0]
    assert int(re.match(r'@@ -0,0 \+1,(\d+) @@', added)[1]) == len(driver.splitlines())
    assert ''.join(l[1:] + '\n' for l in added.splitlines() if l.startswith('+')) == driver
    # Production wiring calls policy before the first step's power/ICC writes.
    assert driver.index('e = s == 1 ? wl_cnss_pdc()') < driver.index('if (!e) { e = wl_icc_step(s); }')
    assert 'python3 tools/test-fuxi-wlan-m6n.py' in (ROOT / 'tools/ci/build.sh').read_text()
    block = '/* ---- milestone 5:' + driver.split('/* ---- milestone 5:', 1)[1].split('static void\nfw_dump(', 1)[0]
    block = re.sub(r'__asm__ volatile\([^;]+;', '/* mocked privileged asm */;', block)
    preamble = m6.PREAMBLE.replace('static void wl_pwr(', 'static void raw_wl_pwr(')
    with tempfile.TemporaryDirectory(prefix='fuxi-wlan-m6n-') as tmp:
        source, binary = Path(tmp) / 'm6n.c', Path(tmp) / 'm6n'
        source.write_text(preamble + PEER + block + m6.TESTS.split('int main(void) {', 1)[0] + TESTS)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-D_DEFAULT_SOURCE', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
