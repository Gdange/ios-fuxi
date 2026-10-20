#!/usr/bin/env python3
"""Host-side checks for the legacy wallpaper step of sweet-root-tweaks.py."""
import importlib.util, os, struct, tempfile, zlib
from pathlib import Path

spec = importlib.util.spec_from_file_location('tweaks', Path(__file__).with_name('sweet-root-tweaks.py'))
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)


def parse_cpbitmap(blob):
    """Mirror of AppSupport CPBitmapCreateImagesFromData for the 28-byte
    trailer, format 0 path (20H392 disassembly)."""
    cs_len, extra_len, w, h, fmt, count, magic = struct.unpack('<7I', blob[-28:])
    assert magic == 0xdcb543a2 and fmt == 0 and count == 1 and cs_len == 0
    stride = (w + 15) // 16 * 16 * 4
    assert stride * h + extra_len + cs_len + 28 == len(blob), 'length check'
    extra = blob[stride * h:stride * h + extra_len]
    return w, h, stride, extra


# Trailer + extra plist exactly as in SpringBoard's own 3x7 highlight cpbitmap.
blob = m.cpbitmap(3, 7, bytes(3 * 7 * 4))
assert len(blob) == 526
assert blob[-78:] == bytes.fromhex(
    '62706c6973743030234008000000000000080000000000000101000000000000000100000000000000000000000000000011'
    '00000000' '32000000' '03000000' '07000000' '00000000' '01000000' 'a243b5dc')

w, h = m.SCREEN
blob = m.cpbitmap(w, h, m.gradient(w, h))
pw, ph, stride, extra = parse_cpbitmap(blob)
assert (pw, ph, stride) == (1125, 2436, 1136 * 4)
first = blob[:4]
last = blob[stride * (h - 1):stride * (h - 1) + 4]
assert first == bytes((0x72, 0x3b, 0x1d, 255)) and last == bytes((0x9a, 0x3d, 0x6a, 255))


def png(w, h, rgb_rows, filters):
    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    raw = b''
    prev = bytes(w * 3)
    for row, f in zip(rgb_rows, filters):
        enc = bytearray(row)
        for i in range(len(row)):
            a = row[i - 3] if i >= 3 else 0
            if f == 1:
                enc[i] = (row[i] - a) & 255
            elif f == 2:
                enc[i] = (row[i] - prev[i]) & 255
        raw += bytes((f,)) + bytes(enc)
        prev = row
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


rows = [bytes(v for x in range(5) for v in (10 * x + y, 20 + y, 200 - x)) for y in range(3)]
with tempfile.TemporaryDirectory() as tmp:
    p = os.path.join(tmp, 'w.png')
    open(p, 'wb').write(png(5, 3, rows, (0, 1, 2)))
    pw, ph, bgra = m.read_png(p)
    assert (pw, ph) == (5, 3)
    for y in range(3):
        for x in range(5):
            r, g, b = rows[y][x * 3:x * 3 + 3]
            assert bgra[(y * 5 + x) * 4:(y * 5 + x) * 4 + 4] == bytes((b, g, r, 255))

    root = os.path.join(tmp, 'root')
    owner = (os.getuid(), os.getgid())
    m.legacy_wallpaper_image(root, p, owner)
    for name in m.WALLPAPER_FILES:
        f = os.path.join(root, m.WALLPAPER_DIR, name)
        assert parse_cpbitmap(open(f, 'rb').read())[:2] == (5, 3)
        assert os.stat(f).st_mode & 0o777 == 0o644
    m.remove_legacy_wallpaper_image(root)
    assert not any(os.path.exists(os.path.join(root, m.WALLPAPER_DIR, n)) for n in m.WALLPAPER_FILES)

print('PASS: cpbitmap trailer/length/pixels, PNG decode, wallpaper files')
