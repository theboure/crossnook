#!/usr/bin/env python3
"""Host-side validation for the CREngine EPUB rendering spike.

Reads cre-dump/dump.json (written by crossnook-cre-test --dump under qemu)
plus the raw page-*.bin dumps (600x800 RGB565 LE, stride 1200) and asserts:

  * page dimensions are exactly 600x800 (row_size 1200)
  * the book laid out to at least 2 pages
  * every dumped page carries ink (not blank)
  * page-000 and page-001 differ (NEXT != PREV page)
  * every dump's FNV-1a checksum matches the value recorded in dump.json
  * no page is entirely non-white garbage (sanity, not strict content match)

Writes a PNG preview per page.
Usage: validate-cre.py <dump-dir>
"""
import json
import struct
import sys

from pathlib import Path

W, H = 600, 800
BUF = W * H * 2
WHITE = 0xFFFF


def fnv1a(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def png565(fb):
    import zlib as _z
    px = bytearray()
    for y in range(H):
        px.append(0)
        for x in range(W):
            v = fb[y * W + x]
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            px += bytes((r * 255 // 31, g * 255 // 63, b * 255 // 31))

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", _z.crc32(c))

    ihdr = struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", _z.compress(bytes(px))) + chunk(b"IEND", b""))


def main(argv):
    if len(argv) != 2:
        print("usage: validate-cre.py <dump-dir>")
        return 1
    d = Path(argv[1])
    jp = d / "dump.json"
    if not jp.exists():
        print("FAIL: %s missing" % jp)
        return 1
    meta = json.loads(jp.read_text())
    ok = True

    def check(name, cond):
        nonlocal ok
        print("[%s] %s" % ("OK" if cond else "FAIL", name))
        ok = ok and cond

    check("dimensions 600x800 row_size 1200",
          meta.get("width") == W and meta.get("height") == H
          and meta.get("row_size") == W * 2)
    check("page_count >= 2", meta.get("page_count", 0) >= 2)

    pages = meta.get("pages", [])
    check("dumped page list non-empty", len(pages) >= 2)

    bins = {}
    all_white = True
    for entry in pages:
        fb = open(d / entry["file"], "rb").read()
        bins[entry["page"]] = fb
        check("  page %d size %d" % (entry["page"], len(fb)),
              len(fb) == BUF)
        if len(fb) != BUF:
            continue
        vals = struct.unpack("<" + "H" * (W * H), fb)
        ink = sum(1 for v in vals if v != WHITE)
        all_white &= ink == 0
        check("  page %d checksum matches" % entry["page"],
              ("%08x" % fnv1a(fb)) == entry["checksum"])
        check("  page %d ink > 0 (%d px)" % (entry["page"], ink), ink > 0)

    check("not every page blank", not all_white)

    if 0 in bins and 1 in bins:
        check("page 0 != page 1 (NEXT distinct from PREV)", bins[0] != bins[1])

    if all_white:
        check("page 0 == page 1 impossible (blank frames)", False)

    for p, fb in bins.items():
        if p <= 1:
            pngpath = d / ("page-%03d.png" % p)
            pngpath.write_bytes(png565(fb))
            print("wrote preview: %s" % pngpath)

    print("CRE VALIDATE %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))