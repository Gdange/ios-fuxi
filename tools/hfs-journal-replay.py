#!/usr/bin/env python3
"""Replay the journal of a journaled HFS+/HFSX volume image, as macOS does at mount.

The iOS root is never cleanly unmounted (the phone is reset), so committed
transactions can still sit in the journal. Linux's hfsplus driver ignores the
journal, so they must be written to their home blocks before a Linux mount.

Format: Apple TN1150 "Journal" and XNU vfs_journal.c (journal_header,
block_list_header, block_info, calc_checksum). The journal header may be in
either byte order; the journal info block is always big-endian.

  hfs-journal-replay.py IMAGE          # check and list only (default)
  hfs-journal-replay.py IMAGE --apply  # write blocks home, mark journal empty

Volume-header helpers for the Linux edit (tools/root-edit-linux.sh), used only
with an empty journal and a clean fsck:
  --mark-unmounted   set kHFSVolumeUnmountedBit (Linux hfsplus mounts read-only
                     without it), as a clean macOS unmount leaves it
  --mark-journaled   after Linux unmounts: put lastMountedVersion back to 'HFSJ'
                     in the primary and alternate headers, so XNU sees a volume
                     last unmounted by a journaling implementation
"""
import argparse, os, struct, sys

JH_MAGIC, ENDIAN_MAGIC = 0x4A4E4C78, 0x12345678
JIB_JOURNAL_IN_FS = 0x1
JH_CKSUM_SIZE, BLH_CKSUM_SIZE, BINFO_SIZE = 44, 32, 16
BLHDR_CHECK_CHECKSUMS = 0x1


def checksum(buf):
    c = 0
    for b in buf:
        c = ((c << 8) ^ (c + b)) & 0xFFFFFFFF
    return (~c) & 0xFFFFFFFF


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--mark-unmounted", action="store_true")
    ap.add_argument("--mark-journaled", action="store_true")
    a = ap.parse_args()
    if a.mark_unmounted or a.mark_journaled:
        return mark(a)
    fd = os.open(a.image, os.O_RDWR if a.apply else os.O_RDONLY)
    rd = lambda off, n: os.pread(fd, n, off)

    vh = rd(1024, 512)
    sig, _, attr, _, jib = struct.unpack(">2sHI4sI", vh[:16])
    bsize = struct.unpack(">I", vh[40:44])[0]
    if sig not in (b"H+", b"HX") or not attr & 0x2000:
        sys.exit(f"not a journaled HFS+ volume (sig {sig}, attributes {attr:#x})")
    ji = rd(jib * bsize, 52)
    jflags = struct.unpack(">I", ji[:4])[0]
    joff, jsize = struct.unpack(">QQ", ji[36:52])
    if not jflags & JIB_JOURNAL_IN_FS:
        sys.exit("journal is not inside the volume")

    raw = rd(joff, 512)
    for e in "<>":
        if struct.unpack(e + "I", raw[:4])[0] == JH_MAGIC:
            break
    else:
        sys.exit("bad journal magic")
    magic, endian, start, end, size, blhdr_size, cks, jhdr_size = struct.unpack(e + "IIQQQIII", raw[:44])
    if endian != ENDIAN_MAGIC or size != jsize:
        sys.exit(f"bad journal header (endian {endian:#x}, size {size} vs {jsize})")
    zeroed = raw[:36] + b"\0" * 4 + raw[40:JH_CKSUM_SIZE]
    if checksum(zeroed) != cks:
        sys.exit("journal header checksum mismatch")
    print(f"journal {'little' if e == '<' else 'big'}-endian start {start} end {end} size {size} "
          f"blhdr {blhdr_size} jhdr {jhdr_size}")
    if start == end:
        print("JOURNAL_EMPTY")
        return

    def jread(pos, n):  # read n bytes at journal offset pos, wrapping past the header
        out = b""
        while n:
            if pos >= size:
                pos = jhdr_size
            k = min(n, size - pos)
            out += rd(joff + pos, k)
            pos, n = pos + k, n - k
        return out, (jhdr_size if pos >= size else pos)

    writes, pos, lists = [], start, 0
    while pos != end:
        hdr, _ = jread(pos, blhdr_size)
        max_blocks, num_blocks, bytes_used, bcks = struct.unpack(e + "HHII", hdr[:12])
        flags = struct.unpack(e + "I", hdr[28:32])[0]  # binfo[0].next carries the flags
        if not 1 <= num_blocks <= max_blocks or not blhdr_size <= bytes_used <= size:
            sys.exit(f"bad block list at {pos}: {num_blocks}/{max_blocks} used {bytes_used}")
        if flags & BLHDR_CHECK_CHECKSUMS:
            z = hdr[:8] + b"\0" * 4 + hdr[12:BLH_CKSUM_SIZE]
            if checksum(z) != bcks:
                sys.exit(f"block list checksum mismatch at {pos}")
        _, dpos = jread(pos, blhdr_size)
        for i in range(1, num_blocks):
            bnum, bsz, _ = struct.unpack(e + "QII", hdr[16 + i * BINFO_SIZE:16 + (i + 1) * BINFO_SIZE])
            data, dpos = jread(dpos, bsz)
            if bnum != 0xFFFFFFFFFFFFFFFF:
                writes.append((bnum * jhdr_size, data))
        _, pos = jread(pos, bytes_used)
        lists += 1

    img_size = os.fstat(fd).st_size
    for off, data in writes:
        if off + len(data) > img_size:
            sys.exit(f"block outside the image at {off}")
    print(f"{lists} block lists, {len(writes)} blocks, {sum(len(d) for _, d in writes)} bytes to replay")
    if not a.apply:
        print("DRY_RUN (use --apply)")
        return
    for off, data in writes:  # in journal order, so later copies win
        os.pwrite(fd, data, off)
    os.fsync(fd)
    hdr = bytearray(raw[:jhdr_size])
    struct.pack_into(e + "Q", hdr, 8, end)  # start = end: journal empty
    struct.pack_into(e + "I", hdr, 36, 0)
    struct.pack_into(e + "I", hdr, 36, checksum(bytes(hdr[:JH_CKSUM_SIZE])))
    os.pwrite(fd, bytes(hdr), joff)
    os.fsync(fd)
    print("JOURNAL_REPLAYED")


def mark(a):
    fd = os.open(a.image, os.O_RDWR)
    vh = os.pread(fd, 512, 1024)
    bsize, total = struct.unpack(">II", vh[40:48])
    heads = [1024, bsize * total - 1024]  # primary, alternate (end of the volume)
    for off in heads:
        h = bytearray(os.pread(fd, 512, off))
        if h[:2] not in (b"H+", b"HX"):
            sys.exit(f"no volume header at {off}")
        if a.mark_unmounted:
            attr = struct.unpack_from(">I", h, 4)[0]
            struct.pack_into(">I", h, 4, attr | 0x100)
        if a.mark_journaled:
            if not struct.unpack_from(">I", h, 4)[0] & 0x2000:
                sys.exit("volume is not journaled")
            h[8:12] = b"HFSJ"
        os.pwrite(fd, bytes(h), off)
    os.fsync(fd)
    print("VOLUME_HEADERS", "unmounted" if a.mark_unmounted else "", "HFSJ" if a.mark_journaled else "")


if __name__ == "__main__":
    main()
