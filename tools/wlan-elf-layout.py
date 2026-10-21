#!/usr/bin/env python3
"""Report ELF/FBC extents without extracting or retaining firmware bytes.

Qualcomm segment kind uses p_flags[26:24]; kind 2 denotes hash metadata.
This checks container bounds, not signatures or undocumented ROM requirements.
"""
import argparse
import json
from pathlib import Path
import struct


def inspect(data, prefix=0x80000):
    if prefix <= 0 or len(data) < 16 or data[:4] != b'\x7fELF':
        raise ValueError('missing ELF identity or invalid prefix')
    cls = data[4]
    if cls not in (1, 2) or data[5:7] != b'\x01\x01':
        raise ValueError('expected ELF32/64 little endian version 1')
    header_size, entry_size = (52, 32) if cls == 1 else (64, 56)
    if len(data) < header_size:
        raise ValueError('truncated ELF header')
    header = struct.unpack_from('<HHIIIIIHHHHHH' if cls == 1 else '<HHIQQQIHHHHHH', data, 16)
    _, machine, version, entry, phoff, _, _, ehsize, phsize, phnum, _, _, _ = header
    phend = phoff + phsize * phnum
    if version != 1 or ehsize != header_size or phsize != entry_size or not phnum:
        raise ValueError('invalid ELF version/header/program entry size or count')
    if phoff < ehsize or phend > len(data):
        raise ValueError('program table outside file')
    segments = []
    for i in range(phnum):
        values = struct.unpack_from('<IIIIIIII' if cls == 1 else '<IIQQQQQQ', data, phoff + i * phsize)
        if cls == 1:
            typ, offset, _, _, size, _, flags, _ = values
        else:
            typ, flags, offset, _, _, size, _, _ = values
        end = offset + size
        if offset > len(data) or end > len(data):
            raise ValueError(f'segment {i} outside file')
        segments.append(dict(index=i, type=typ, flags=flags, kind=(flags >> 24) & 7,
                             offset=offset, filesz=size, end=end, fits_prefix=end <= prefix))
    hashes = [s for s in segments if s['kind'] == 2]
    # MBN v5 is a 40-byte header followed by hashes and optional QTI/OEM
    # signature/certificate material. Report lengths only; never extract it.
    hash_headers = []
    for seg in hashes:
        if seg['filesz'] < 40:
            hash_headers.append(dict(version=None, parsed=False))
            continue
        h = struct.unpack_from('<10I', data, seg['offset'])
        if h[1] != 5:
            hash_headers.append(dict(version=h[1], parsed=False))
            continue
        used = h[2] + h[3] + h[5] + h[7] + h[9]
        hash_headers.append(dict(version=5, parsed=True, image_size=h[4], code_size=h[5],
                                 qti_signature_size=h[2], qti_certificate_size=h[3],
                                 oem_signature_size=h[7], oem_certificate_size=h[9],
                                 lengths_fit=used <= seg['filesz'] - 40,
                                 authentication='unverified'))
    return dict(elf_class=cls * 32, machine=machine, entry=entry, file_size=len(data),
                prefix=prefix, phoff=phoff, phnum=phnum, phend=phend,
                metadata_fits=phend <= prefix and bool(hashes) and
                all(s['filesz'] and s['fits_prefix'] for s in hashes),
                hash_count=len(hashes), hash_headers=hash_headers, segments=segments)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('images', nargs='+', type=Path)
    parser.add_argument('--prefix', type=lambda x: int(x, 0), default=0x80000)
    args = parser.parse_args()
    for path in args.images:
        try:
            result = inspect(path.read_bytes(), args.prefix)
        except (OSError, ValueError) as exc:
            parser.exit(1, f'{path.name}: {exc}\n')
        print(json.dumps(dict(image=path.name, **result), sort_keys=True))


if __name__ == '__main__':
    main()
