#!/usr/bin/env python3
"""Inspect a Fuxi FDT before choosing XNU memory/MMIO parameters.

Accepts a live DTB or a concatenated vendor_boot DTB bundle. Does not generate
a bootable ADT: stock trees omit ABL's runtime RAM and splash fixups.
"""
import argparse
import json
import struct
from pathlib import Path


def parse(data):
    if len(data) < 40:
        raise ValueError('truncated FDT header')
    magic, total, off_struct, off_strings, off_reserve, version, compatible, _, size_strings, size_struct = struct.unpack_from('>10I', data)
    if magic != 0xd00dfeed or not 17 <= version or compatible > 17:
        raise ValueError('expected an FDT v17-compatible blob')
    if total > len(data) or total < 40:
        raise ValueError('invalid FDT total size')
    for offset, size in ((off_struct, size_struct), (off_strings, size_strings)):
        if offset < 40 or offset + size > total:
            raise ValueError('FDT section outside blob')
    strings = data[off_strings:off_strings + size_strings]
    limit = off_struct + size_struct
    pos, stack, nodes = off_struct, [], {}

    def word():
        nonlocal pos
        if pos + 4 > limit:
            raise ValueError('truncated FDT structure')
        value, = struct.unpack_from('>I', data, pos)
        pos += 4
        return value

    while pos < limit:
        token = word()
        if token == 1:
            end = data.find(b'\0', pos, limit)
            if end < 0:
                raise ValueError('unterminated node name')
            name = data[pos:end].decode('ascii')
            pos = (end + 4) & ~3
            path = (stack[-1].rstrip('/') + '/' + name) if stack else '/'
            if path in nodes:
                raise ValueError('duplicate node path')
            nodes[path] = {}
            stack.append(path)
        elif token == 2:
            if not stack:
                raise ValueError('unbalanced FDT node')
            stack.pop()
        elif token == 3:
            length, nameoff = word(), word()
            end = strings.find(b'\0', nameoff)
            if not stack or nameoff >= len(strings) or end < 0 or pos + length > limit:
                raise ValueError('invalid FDT property')
            name = strings[nameoff:end].decode('ascii')
            nodes[stack[-1]][name] = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            if stack or '/' not in nodes:
                raise ValueError('unbalanced FDT tree')
            break
        else:
            raise ValueError(f'invalid FDT token {token}')
    else:
        raise ValueError('missing FDT end token')
    reserves = []
    if off_reserve < 40 or off_reserve % 8:
        raise ValueError('invalid FDT reserve map')
    while off_reserve + 16 <= total:
        address, size = struct.unpack_from('>QQ', data, off_reserve)
        off_reserve += 16
        if address == size == 0:
            break
        reserves.append({'address': hex(address), 'size': hex(size)})
    else:
        raise ValueError('unterminated FDT reserve map')
    return nodes, reserves, total


def cells(raw):
    if len(raw) % 4:
        raise ValueError('property is not a cell array')
    return list(struct.unpack('>%dI' % (len(raw) // 4), raw))


def describe(nodes, reserves):
    def decode(name, raw):
        if name in ('compatible', 'device_type', 'reg-names', 'method', 'status',
                    'qcom,mdss-dsi-panel-type') and raw.endswith(b'\0'):
            return raw[:-1].decode('ascii').split('\0')
        return [hex(v) for v in cells(raw)] if len(raw) % 4 == 0 else raw.hex()

    selected = {}
    for path, props in nodes.items():
        compatible = props.get('compatible', b'').lower()
        if not (path in ('/', '/chosen', '/memory', '/reserved-memory') or
                path.startswith(('/cpus/', '/memory@', '/reserved-memory/')) or
                any(s in compatible for s in (b'gic', b'timer', b'psci', b'ufshc',
                    b'msm-imem', b'dsi', b'spi', b'geni', b'spmi'))):
            continue
        keep = ('compatible', 'device_type', 'reg', 'reg-names', 'interrupts',
                'interrupt-parent', 'clock-frequency', 'redistributor-stride',
                'method', 'no-map', 'reusable', 'size', 'alignment', 'alloc-ranges',
                'status', 'qcom,mdss-dsi-panel-type', 'qcom,mdss-dsi-panel-width',
                'qcom,mdss-dsi-panel-height', '#address-cells', '#size-cells')
        selected[path] = {k: decode(k, v) for k, v in props.items() if k in keep}
        if 'reg' in props and path != '/':
            parent = nodes[path.rsplit('/', 1)[0] or '/']
            ac = cells(parent.get('#address-cells', struct.pack('>I', 2)))[0]
            sc = cells(parent.get('#size-cells', struct.pack('>I', 1)))[0]
            values = cells(props['reg'])
            width = ac + sc
            if width and ac <= 2 and sc <= 2 and len(values) % width == 0:
                def number(v):
                    result = 0
                    for cell in v:
                        result = result << 32 | cell
                    return hex(result)
                selected[path]['decoded-reg'] = [
                    {'address': number(values[i:i + ac]),
                     'size': number(values[i + ac:i + width])}
                    for i in range(0, len(values), width)]
    return {'warning': 'Static DTBs lack ABL runtime fixups; no RAM range is certified safe. '
                       'Decoded reg addresses are relative to the parent bus; apply ranges for physical addresses.',
            'reserve-map': reserves, 'nodes': selected}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dtb', type=Path)
    parser.add_argument('--index', type=int, default=0, help='index in a concatenated DTB bundle')
    args = parser.parse_args()
    if args.index < 0:
        parser.error('--index must be nonnegative')
    data = args.dtb.read_bytes()
    try:
        for index in range(args.index + 1):
            nodes, reserves, total = parse(data)
            if index != args.index:
                data = data[total:]
        print(json.dumps(describe(nodes, reserves), indent=2))
    except (ValueError, UnicodeDecodeError, struct.error) as error:
        parser.exit(1, f'{args.dtb}: {error}\n')


if __name__ == '__main__':
    main()
