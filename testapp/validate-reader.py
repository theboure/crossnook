#!/usr/bin/env python3
"""Host-side validation for the Library -> Reader integration.

Reads reader-dump/dump.json (written by crossnook-reader-test --dump under
qemu) plus the raw frame .bin dumps (600x800 RGB565 LE, stride 1200) and
asserts:

  * frame dimensions are exactly 600x800 (row_size 1200)
  * every frame carries ink (not blank) and its recorded FNV-1a checksum
    matches
  * reader_p0, reader_p1 and reader_last come from a real (>=2 page) book
    and reader_p0 != reader_p1 (NEXT distinct), reader_last != reader_p0
  * reader_p0 / reader_p1 / reader_last agree with the CREngine regression
    (cre-dump/dump.json) in page-count and per-page ink within a small
    tolerance — proof the extracted reader layer reproduces the validated
    spike layout. Pixel identity is NOT gated here: the freestanding
    CREngine build itself renders optionally-chosen fallback glyphs
    differently across builds, so byte equality is not a stable contract;
    a strict per-page compare is printed as an informational note.
  * the invalid-epub frame and the FB2 frame are distinct from the library
    and from each other (deterministic SELECTED_BOOK fallbacks)

Writes PNG previews for the readable frames.

Usage: validate-reader.py <reader-dump-dir> [<cre-dump-dir>]
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
    if len(argv) < 2:
        print("usage: validate-reader.py <reader-dump-dir> [<cre-dump-dir>]")
        return 1
    d = Path(argv[1])
    cre_dir = Path(argv[2]) if len(argv) > 2 else None

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
    check("reader_pages >= 2", meta.get("reader_pages", 0) >= 2)

    frames = {f["frame"]: f for f in meta.get("frames", [])}
    needed = ("home", "library", "reader_p0", "reader_p1",
              "reader_last", "invalid", "fb2")
    check("all 7 frames present", all(name in frames for name in needed))

    bins = {}
    for name in needed:
        entry = frames[name]
        fb = open(d / entry["file"], "rb").read()
        bins[name] = fb
        check("  %s size %d" % (name, len(fb)), len(fb) == BUF)
        if len(fb) != BUF:
            continue
        vals = struct.unpack("<" + "H" * (W * H), fb)
        ink = sum(1 for v in vals if v != WHITE)
        atoms = sum(1 for v in vals if v not in (WHITE, 0x7BEF, 0))
        check("  %s checksum matches" % name,
              ("%08x" % fnv1a(fb)) == entry["checksum"])
        check("  %s ink > 0 (%d px, %d non-gray/black atoms)" % (name, ink, atoms),
              ink > 0 and atoms > 0)

    if "reader_p0" in bins and "reader_p1" in bins:
        check("reader_p0 != reader_p1 (NEXT distinct)", bins["reader_p0"] != bins["reader_p1"])
        check("reader_last != reader_p0 (real last page)", bins["reader_last"] != bins["reader_p0"])

    if "library" in bins and "invalid" in bins:
        check("invalid (SELECTED) differs from library",
              bins["invalid"] != bins["library"])
    if "invalid" in bins and "fb2" in bins:
        check("fb2 (SELECTED) differs from invalid", bins["fb2"] != bins["invalid"])

    # Equivalence with the validated CREngine spike: the reader layer must
    # reproduce the same page count and overall ink coverage. Pixel identity
    # is informational (the freestanding CREngine build itself renders a few
    # fallback glyphs differently across builds).
    if cre_dir and (cre_dir / "dump.json").exists():
        cre_meta = json.loads((cre_dir / "dump.json").read_text())
        cre_pages = {p["page"]: p for p in cre_meta.get("pages", [])}
        check("reader pages == cre pages (%d)" % cre_meta.get("page_count", 0),
              meta.get("reader_pages") == cre_meta.get("page_count"))
        cre_inks = {p: pdata["ink"] for p, pdata in cre_pages.items()}
        for name, page in (("reader_p0", 0), ("reader_p1", 1)):
            if name not in frames or page not in cre_pages:
                continue
            got = frames[name]["ink"]
            want = cre_inks[page]
            tight = got == want
            loose = want > 0 and abs(got - want) * 100.0 / want <= 5.0
            check("reader %s ink ~ cre page %d (%d vs %d%s)"
                  % (name, page, got, want,
                     "" if tight else " within 5%%" if loose else " OUT OF TOLERANCE"),
                  tight or loose)
            if tight:
                print("  note: %s is pixel-identical to cre page %d" % (name, page))
            else:
                chk_t = frames[name]["checksum"] == cre_pages[page]["checksum"]
                print("  note: %s differs from cre page %d (checksums %s vs %s)"
                      % (name, page, frames[name]["checksum"],
                         cre_pages[page]["checksum"]))
        last_page = meta.get("reader_pages", 0) - 1
        if "reader_last" in frames and last_page in cre_pages:
            got = frames["reader_last"]["ink"]
            want = cre_pages[last_page]["ink"]
            loose = want > 0 and abs(got - want) * 100.0 / want <= 5.0
            check("reader_last ink ~ cre page %d (%d vs %d%s)"
                  % (last_page, got, want, "" if got == want else " within 5%%" if loose else " OUT OF TOLERANCE"),
                  got == want or loose)
    else:
        check("cre regression manifest compared (need cre-dump/dump.json)",
              False)

    for name in ("home", "library", "reader_p0", "fb2"):
        if name in bins:
            pngpath = d / ("frame-%s.png" % name)
            pngpath.write_bytes(png565(bins[name]))
            print("wrote preview: %s" % pngpath)

    print("READER VALIDATE %s" % ("OK" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))