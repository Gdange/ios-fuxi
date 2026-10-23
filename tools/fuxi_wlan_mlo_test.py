"""Shared W12/W15 host-only PHY/HOST_CAP EI fixtures.

Protocol facts: Linux v6.12 ath12k qmi.c/qmi.h, core.c and hw.c;
BSD-3-Clause-Clear, The Linux Foundation (2018-2021), Qualcomm Innovation
Center, Inc. (2021-2024). Independent metadata/encoder, no Linux helpers.
"""
import re
import struct

# All HOST_CAP fields in EI order, including every omitted optional field.
# (name, TLV, scalar width or schema kind, static array limit)
HOST = [
    ('num_clients', 0x10, 4, 1), ('wake_msi', 0x11, 4, 1),
    ('gpios', 0x12, 'GPIO', 32), ('nm_modem', 0x13, 1, 1),
    ('bdf_support', 0x14, 1, 1), ('bdf_cache_support', 0x15, 1, 1),
    ('m3_support', 0x16, 1, 1), ('m3_cache_support', 0x17, 1, 1),
    ('cal_filesys_support', 0x18, 1, 1), ('cal_cache_support', 0x19, 1, 1),
    ('cal_done', 0x1a, 1, 1), ('mem_bucket', 0x1b, 4, 1),
    ('mem_cfg_mode', 0x1c, 1, 1), ('cal_duraiton', 0x1d, 2, 1),
    ('platform_name', 0x1e, 'STRING', 65), ('ddr_range', 0x1f, 'DDR', 3),
    ('host_build_type', 0x20, 'ENUM', 1), ('mlo_capable', 0x21, 1, 1),
    ('mlo_chip_id', 0x22, 2, 1), ('mlo_group_id', 0x23, 1, 1),
    ('max_mlo_peer', 0x24, 2, 1), ('mlo_num_chips', 0x25, 1, 1),
    ('mlo_chip_info', 0x26, 'CHIPS', 3), ('feature_list', 0x27, 8, 1)]
PHY = [('resp', 2, 4), ('num_phy', 0x10, 1), ('board_id', 0x11, 4),
       ('single_chip_mlo_support', 0x13, 1)]
CHIP = [('chip_id', 1), ('num_local_links', 1), ('hw_link_id', 2), ('valid_mlo_link_id', 2)]


def tlv(tag, body):
    return struct.pack('<BH', tag, len(body)) + body


def host_body(links):
    values = dict(num_clients=1, nm_modem=14, bdf_support=1, m3_support=1,
                  m3_cache_support=1, cal_done=0, mem_cfg_mode=0, feature_list=24)
    if links:
        values.update(mlo_capable=1, mlo_chip_id=0, mlo_group_id=0,
                      max_mlo_peer=32, mlo_num_chips=1,
                      mlo_chip_info=[(0, links, bytes(range(links))+bytes(2-links),
                                     bytes([1])*links+bytes(2-links)),
                                    (0, 0, bytes(2), bytes(2)), (0, 0, bytes(2), bytes(2))])
    out = b''
    for name, tag, kind, count in HOST:
        if name not in values:
            continue
        v = values[name]
        if kind == 'CHIPS':
            assert len(v) == count
            body = b''.join(bytes([chip[0], chip[1]])+chip[2]+chip[3] for chip in v)
        else:
            body = v.to_bytes(kind, 'little')
        out += tlv(tag, body)
    return out


def phy_body(num=2, support=1, board=0x12345678, result=0, error=0):
    values = dict(resp=struct.pack('<HH', result, error), num_phy=num,
                  board_id=board, single_chip_mlo_support=support)
    return b''.join(tlv(tag, v if isinstance(v, bytes) else v.to_bytes(width, 'little'))
                    for name, tag, width in PHY if (v := values[name]) is not None)


# variant, response body (None = timeout), expected MLO link count (0 = omitted)
CASES = [(0, b'', 2), (0, b'', 2), (2, b'', 0),
         (1, phy_body(), 2), (1, phy_body(num=1), 1),
         (1, phy_body(support=0), 0), (1, phy_body(support=None), 2),
         (1, phy_body(num=None, support=0), 0), (1, phy_body(num=None), 2),
         (1, phy_body(num=0), 0), (1, phy_body(num=255), 0),
         (1, phy_body(num=1, support=0, result=1, error=94), 2),
         (1, phy_body(num=None, support=None, board=None), 2), (1, None, 2),
         (1, phy_body(num=1)+tlv(0x70, b'\x42'), 1),
         (1, phy_body(num=1, support=0)+tlv(0x10, b'\x02'), 2),
         (1, phy_body()[:-1], 2), (1, phy_body()[7:], 2),
         (1, phy_body(num=1, support=3), 1), (1, phy_body(num=1, board=None), 1),
         (1, phy_body(num=3), -1), (1, phy_body(num=254), -1),
         (1, phy_body(num=3, support=0), 0),
         # ath12k PHY checks result only, reports error without rejecting it.
         (1, phy_body(num=1, error=90), 1)]


def check_upstream(path):
    qmi = (path/'qmi.c').read_text()
    qh = (path/'qmi.h').read_text()
    hw = (path/'hw.c').read_text()
    def entries(name):
        table = qmi.split(name+'[] = {', 1)[1].split('\n};', 1)[0]
        return re.findall(r'\{(.*?)\}', table, re.S)
    host = entries('qmi_wlanfw_host_cap_req_msg_v01_ei')
    assert len(host) == 2*len(HOST)+2  # GPIO DATA_LEN, EOTI
    for name, tag, kind, count in HOST:
        entry = next(e for e in host if re.search(r',\s*'+name+r'\)', e))
        assert int(re.search(r'\.tlv_type\s*=\s*(0x[\da-fA-F]+)', entry)[1], 16) == tag
        if isinstance(kind, int):
            assert 'QMI_UNSIGNED_'+str(kind)+'_BYTE' in entry
        elif kind in ('CHIPS', 'DDR'):
            assert 'QMI_STRUCT' in entry and 'STATIC_ARRAY' in entry
        elif kind == 'ENUM':
            assert 'QMI_SIGNED_4_BYTE_ENUM' in entry
        elif kind == 'STRING':
            assert 'QMI_STRING' in entry
        else:
            assert 'VAR_LEN_ARRAY' in entry and 'QMI_UNSIGNED_4_BYTE' in entry
        flag = 'cal_duration_valid' if name == 'cal_duraiton' else name+'_valid'
        opt = next(e for e in host if re.search(r',\s*'+flag+r'\)', e))
        assert 'QMI_OPT_FLAG' in opt and f'0x{tag:02X}'.lower() in opt.lower()
    constants = {k: int(v, 0) for k, v in re.findall(r'^#define\s+(\w+)\s+(0x[0-9a-fA-F]+|\d+)\s*$', qh, re.M)}
    def count_of(entry):
        expr = re.search(r'\.elem_len\s*=\s*([^,]+)', entry)[1].strip()
        return sum(constants[t.strip()] if t.strip() in constants else int(t.strip(), 0)
                   for t in expr.split('+'))
    for name, tag, kind, count in HOST:
        e = next(e for e in host if re.search(r',\s*'+name+r'\)', e))
        assert count_of(e) == count
    request = entries('qmi_wlanfw_phy_cap_req_msg_v01_ei')
    assert len(request) == 1 and 'QMI_EOTI' in request[0]
    assert constants['QMI_WLANFW_PHY_CAP_REQ_V01'] == 0x57
    assert constants['QMI_WLANFW_PHY_CAP_REQ_MSG_V01_MAX_LEN'] == 0
    phy = entries('qmi_wlanfw_phy_cap_resp_msg_v01_ei')
    assert len(phy) == 8
    for name, tag, width in PHY:
        e = next(e for e in phy if re.search(r',\s*'+name+r'\)', e))
        assert int(re.search(r'\.tlv_type\s*=\s*(0x[\da-fA-F]+)', e)[1], 16) == tag
        assert ('QMI_STRUCT' if name == 'resp' else 'QMI_UNSIGNED_'+str(width)+'_BYTE') in e
        assert count_of(e) == 1 and 'NO_ARRAY' in e
        if name != 'resp':
            opt = next(e for e in phy if re.search(r',\s*'+name+r'_valid\)', e))
            assert 'QMI_OPT_FLAG' in opt
            assert int(re.search(r'\.tlv_type\s*=\s*(0x[\da-fA-F]+)', opt)[1], 16) == tag
    chips = entries('wlfw_host_mlo_chip_info_s_v01_ei')
    for (name, count), e in zip(CHIP, chips):
        assert re.search(r',\s*'+name+r'\)', e) and 'QMI_UNSIGNED_1_BYTE' in e
        assert ('STATIC_ARRAY' if count == 2 else 'NO_ARRAY') in e
        assert count_of(e) == count
    for constant, value in [('QMI_WLFW_MAX_NUM_MLO_CHIPS_V01', 3),
                            ('QMI_WLFW_MAX_NUM_MLO_LINKS_PER_CHIP_V01', 2),
                            ('ATH12K_QMI_TARGET_MEM_MODE_DEFAULT', 0)]:
        assert re.search(r'#define\s+'+constant+r'\s+'+str(value)+r'\b', qh)
    for name, value in [('def_num_link', 2), ('max_mlo_peer', 32)]:
        params = hw.split('.name = "wcn7850 hw2.0"', 1)[1].split('\n\t},', 1)[0]
        assert re.search(r'\.'+name+r'\s*=\s*'+str(value)+r'\b', params)
    assert 'ab->mlo_capable_flags = ATH12K_INTRA_DEVICE_MLO_SUPPORT;' in (path/'core.c').read_text()
    print('PHY/HOST_CAP EI fields, optional flags, static arrays and WCN7850 defaults verified')


def c_array(name, data):
    return 'static const uint8_t '+name+'[]={'+','.join(map(str, data or b'\0'))+'};\n'


def tests(source, w15=False):
    """Wrap the existing transport peer; check real bytes before normalization.

    Normalization lets the original peer keep checking memory/download/CFG
    behavior with its historical transaction IDs and two-link HOST_CAP fixture.
    Every new HOST_CAP and PHY_CAP is checked independently first.
    """
    source = source.split('int main(void) {', 1)[0]
    declarations = 'static unsigned mlo_case, phy_requests, host_requests;\n'
    declarations += 'static const int mlo_variants[]={'+','.join(str(v) for v, _, _ in CASES)+'};\n'
    for i, (_, body, links) in enumerate(CASES):
        declarations += c_array('phy'+str(i), body)
        declarations += c_array('host'+str(i), host_body(max(0, links)))
    declarations += 'static const uint8_t *phy_bodies[]={'+','.join('phy'+str(i) for i in range(len(CASES)))+'};\n'
    declarations += 'static const unsigned phy_sizes[]={'+','.join(str(len(b or b'')) for _, b, _ in CASES)+'};\n'
    declarations += 'static const uint8_t *host_bodies[]={'+','.join('host'+str(i) for i in range(len(CASES)))+'};\n'
    declarations += 'static const unsigned host_sizes[]={'+','.join(str(len(host_body(max(0, n)))) for _, _, n in CASES)+'};\n'
    declarations += c_array('default_host', host_body(2))
    source = source.replace('static void dev_qrtr(', 'static void dev_qrtr_raw(', 1)
    pos = source.index('static void dev_control(')
    source = source[:pos]+declarations+r'''
static void dev_qrtr(unsigned type,const uint8_t *data,unsigned len,bool compact,bool confirm) {
    uint8_t copy[4096];assert(len<=sizeof(copy));memcpy(copy,data,len);
    if(type==1 && len>=7 && copy[0]==2 && mlo_arg==1)put16(copy+1,(copy[1]|copy[2]<<8)+1);
    dev_qrtr_raw(type,copy,len,compact,confirm);
}
'''+source[pos:]
    source = source.replace('static void qmi_peer(', 'static void qmi_base_peer(', 1)
    pos = source.index('static void dev_work(void) {')
    source = source[:pos]+r'''
static void qmi_peer(const uint8_t *q,unsigned len) {
    unsigned msg=q[3]|q[4]<<8, txn=q[1]|q[2]<<8;
    if(msg==0x57) {
        const uint8_t request[]={0,1,0,0x57,0,0,0};
        assert(mlo_arg==1 && !phy_requests++ && !host_requests);
        assert(len==sizeof(request) && !memcmp(q,request,len));
        if(mlo_case==13)return;
        uint8_t response[128]={2,1,0,0x57,0};unsigned n=phy_sizes[mlo_case];
        put16(response+5,n);memcpy(response+7,phy_bodies[mlo_case],n);
        /* Wrong transaction/message cannot complete PHY_CAP. */
        response[1]=9;dev_qrtr_raw(1,response,n+7,false,false);response[1]=1;
        response[3]=0x58;dev_qrtr_raw(1,response,n+7,false,false);response[3]=0x57;
        dev_qrtr_raw(1,response,n+7,false,false);return;
    }
    uint8_t copy[8192];assert(len<=sizeof(copy));memcpy(copy,q,len);
    if(mlo_arg==1) {assert(phy_requests==1 && txn>1);put16(copy+1,txn-1);}
    if(msg==0x34) {
        assert(!host_requests++ && len==7+host_sizes[mlo_case]);
        assert(!memcmp(q+7,host_bodies[mlo_case],len-7));
        len=7+sizeof(default_host);put16(copy+5,sizeof(default_host));
        memcpy(copy+7,default_host,sizeof(default_host));
    }
    qmi_base_peer(copy,len);
}
'''+source[pos:]
    source = source.replace('stop22_arg=0;keep_arg=keep;qrtr_arg=',
                            'mlo_present=mlo_case!=0;mlo_arg=mlo_variants[mlo_case];stop22_arg=0;keep_arg=keep;qrtr_arg=')
    setup = 'boot15(0,true);' if w15 else 'begin(0,true);'
    main = r'''
int main(void) {
    /* Unknown/negative selectors refuse W12+; ignored before W12. */
    for(int v=-1;v<=3;v++) {
        qrtr_arg=2;mlo_present=true;mlo_arg=v;wl_probe_configure();assert(wl_variant_invalid==(v<0||v>2));
        qrtr_arg=1;wl_probe_configure();assert(!wl_variant_invalid && !wl12_mlo_variant);
    }
    const unsigned variants[]={VARIANTS};
    for(mlo_case=0;mlo_case<sizeof(variants)/sizeof(variants[0]);mlo_case++) {
        SETUP
        assert(mlo_arg==variants[mlo_case] && wl12_mlo_variant==variants[mlo_case]);phy_requests=host_requests=0;
        unsigned ticks=sleeps;int error=finish_step();
        if(mlo_case==20 || mlo_case==21) {
            assert(error && !wl_w11_ok && !host_requests);
            assert(strstr(logbuf,"exceeds two-link EI capacity"));contain();continue;
        }
        if(error)fprintf(stderr,"case %u\n%s",mlo_case,logbuf);
        assert(!error && wl_w11_ok && host_requests==1);
        assert(phy_requests==(mlo_arg==1));
        assert(wl12_txn==LAST_TXN+(mlo_arg==1));
        if(mlo_case==13)assert(sleeps-ticks>=10000 && strstr(logbuf,"timeout 10000 ms"));
        if(mlo_case==4)assert(strstr(logbuf,"num_phy valid 1 value 1 board_id valid 1 value 0x12345678"));
        if(mlo_case==7)assert(strstr(logbuf,"absent num_phy") && !wl12_mlo_enabled);
        if(mlo_case==15 || mlo_case==16 || mlo_case==17)assert(strstr(logbuf,"malformed reply"));
        CHECK_W15
        contain();
    }
    /* Malformed fields are atomic: no partial support/num update. */
    mlo_arg=1;wl12_mlo_variant=1;wl12_txn=1;wl12_msg=0x57;
    uint8_t response[128]={2,1,0,0x57,0};
    unsigned n=phy_sizes[3];memcpy(response+7,phy_bodies[3],n);put16(response+5,n);
    for(unsigned cut=7;cut<n+7;cut++) {
        wl12_reply=false;wl12_num_phy=2;wl12_mlo_enabled=true;clear_error();
        assert(!wl_qmi_w12(response,cut) && wl12_reply && !wl_w11_error);
        assert(wl12_num_phy==2 && wl12_mlo_enabled);
    }
    const unsigned offsets[]={7,14,18,25}, widths[]={4,1,4,1};
    for(unsigned i=0;i<4;i++) {
        for(unsigned width=0;width<=5;width++) {
            if(width==widths[i])continue;
            unsigned off=offsets[i];memcpy(response+7,phy_bodies[3],n);
            put16(response+off+1,width);put16(response+5,n);
            wl12_reply=false;wl12_num_phy=2;wl12_mlo_enabled=true;clear_error();
            assert(!wl_qmi_w12(response,n+7) && wl12_reply && !wl_w11_error);
            assert(wl12_num_phy==2 && wl12_mlo_enabled);
        }
        memcpy(response+7,phy_bodies[3],n);
        memcpy(response+7+n,response+offsets[i],3+widths[i]);put16(response+5,n+3+widths[i]);
        wl12_reply=false;wl12_num_phy=2;wl12_mlo_enabled=true;clear_error();
        assert(!wl_qmi_w12(response,7+n+3+widths[i]) && wl12_reply);
        assert(wl12_num_phy==2 && wl12_mlo_enabled);
    }
    puts("LANE PHY_CAP/HOST_CAP EI bytes, full variants, fallback, bounds and malformed reply tests passed");
    return 0;
}
'''
    main = main.replace('VARIANTS', ','.join(str(v) for v, _, _ in CASES)).replace('SETUP', setup)
    main = main.replace('LAST_TXN', '15' if w15 else '4').replace('CHECK_W15', 'assert(seen15==3);check_no_ce();' if w15 else '')
    return source + main.replace('LANE', 'W15' if w15 else 'W12')
