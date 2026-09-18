#!/usr/bin/env python3
"""Host-side validation of crossnook-text's framebuffer output.

Checks (against the 960000-byte RGB565 dump, 600x800, stride 1200):
  1. file is exactly 960000 bytes
  2. not all-white (ink pixels present), not all-black (bg pixels present)
  3. ink stays within screen bounds
  4. Latin and Cyrillic sample lines both have ink in their y-bands
  5. all four size samples render the FULL phrase via the multiline
     wrap path (expected wrapped-line count + ink + right-edge extent),
     so truncated/single-line samples fail loudly
  6. title region ("CrossNook") is legible in a coarse ASCII preview

Writes <in>.png preview alongside the dump.
"""
import struct
import sys


def main(path):
    data = open(path, "rb").read()
    W, H = 600, 800
    assert len(data) == W * H * 2, f"size {len(data)} != 960000"
    fb = struct.unpack("<" + "H" * (W * H), data)

    def ink(x, y):
        return fb[y * W + x] < 0x8000  # dark-ish pixel (antialiased text)

    # global stats
    ink_px = 0
    minx, miny, maxx, maxy = W, H, -1, -1
    for y in range(H):
        for x in range(W):
            if ink(x, y):
                ink_px += 1
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y

    ok = True
    def check(name, cond):
        nonlocal ok
        print(f"[{'OK' if cond else 'FAIL'}] {name}")
        if not cond:
            ok = False

    check(f"size == 960000", len(data) == 960000)
    check(f"not all-white (ink_px={ink_px} > 500)", ink_px > 500)
    check(f"not all-black (white_px={W*H-ink_px} > 100000)",
          W * H - ink_px > 100000)
    check(f"bbox ({minx},{miny})-({maxx},{maxy}) inside (0,0)-(600,800)",
          0 <= minx and maxx < W and 0 <= miny and maxy < H)

    # band analysis: count ink per 10px y-band to locate Latin/Cyrillic/sizes
    bands = {}
    for y in range(H):
        for x in range(W):
            if ink(x, y):
                bands[y // 10] = bands.get(y // 10, 0) + 1

    # top: title(~33-80), caption(~90-104), latin(~120-146), cyrillic(~150-200)
    def band_ink(y0, y1):
        return sum(v for b, v in bands.items() if b * 10 < y1 and b * 10 + 10 > y0)

    # deterministic layout for DejaVu Sans (see docs): title ~25-61,
    # caption ~128-141, latin 24px ~165-217 (2 lines), cyrillic 24px
    # ~225-277 (2 wrapped lines), then a size legend and four samples of
    # the SAME full phrase wrapped by render_block: 18px=1 line, 24px=2,
    # 32px=2, 48px=3 lines, completing at y~751 (fully visible).
    latin_band = band_ink(165, 218)
    cyr_band = band_ink(225, 278)
    check(f"Latin line(s) have ink (latin_band={latin_band} > 500)", latin_band > 500)
    check(f"Cyrillic line(s) have ink (cyr_band={cyr_band} > 500)", cyr_band > 500)
    check(f"all content fully visible (maxy={maxy} < 780)", maxy < 780)

    def region_stats(y0, y1):
        # contiguous text-row groups (wrapped lines), total ink,
        # rightmost ink column
        rows = [y for y in range(y0, y1)
                if any(ink(x, y) for x in range(W))]
        groups = 0
        prev = -10
        for y in rows:
            if y - prev > 3:
                groups += 1          # a new (wrapped) line starts here
            prev = y
        ink_n = sum(1 for y in range(y0, y1) for x in range(W)
                    if ink(x, y))
        mx = max((x for y in range(y0, y1) for x in range(W)
                  if ink(x, y)), default=-1)
        return groups, ink_n, mx

    # Every size must render the full phrase "The quick brown fox jumps
    # over the lazy dog." through the multiline wrap path: the expected
    # wrapped line count, a minimum ink floor, and a rightward ink extent
    # proving the phrase tail reached the end of its last line (a sample
    # truncated early — no continuation lines — would fail line count /
    # right-edge checks).
    sizes = {
        18: dict(range=(339, 356), lines=1, min_ink=1400, min_maxx=350),
        24: dict(range=(383, 435), lines=2, min_ink=2400, min_maxx=450),
        32: dict(range=(469, 539), lines=2, min_ink=4000, min_maxx=450),
        48: dict(range=(584, 751), lines=3, min_ink=8000, min_maxx=400),
    }
    for px, s in sizes.items():
        groups, ink_n, mx = region_stats(*s["range"])
        cond = (groups == s["lines"] and ink_n > s["min_ink"]
                and mx > s["min_maxx"])
        check(f"{px}px sample: full phrase wrapped "
              f"(lines={groups}/{s['lines']} ink={ink_n}>{s['min_ink']} "
              f"right={mx}>{s['min_maxx']})", cond)

    # ASCII preview of the title region
    print("\n--- title region (downsampled 4x, '#'=ink) ---")
    t0 = max(0, miny - 6)
    t1 = miny + 58
    for y in range(t0, t1, 4):
        row = ""
        for x in range(0, maxx + 1, 4):
            row += "#" if any(ink(x + dx, y + dy) for dx in range(4)
                              for dy in range(4) if 0 <= x + dx < W and y + dy < H) else "."
        print(row)

    # write PNG preview
    pngpath = path.replace(".bin", ".png")
    with open(pngpath, "wb") as f:
        f.write(png565(fb, W, H))
    print(f"\nwrote preview: {pngpath}")

    sys.exit(0 if ok else 1)


def png565(fb, W, H):
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


if __name__ == "__main__":
    main(sys.argv[1])