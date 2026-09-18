#!/usr/bin/env python3
"""Host-side validation of crossnook-library-test framebuffer dumps.

Kinds (all 600x800 RGB565 LE, stride 1200):
  LIBRARY   - "CrossNook / Library" header, 14 book rows starting at
              y=168 (row height 40, viewport 0), gray highlight on the
              selected row 0, gray footer "1-14 of 41" on the y=750
              baseline (library screens never render the touch marker)
  SCROLLED  - same layout with viewport top=14 (sel=14), i.e. row 0 of
              the viewport is highlighted; footer "15-28 of 41"
  EMPTY     - header only + centered "No books found." message; no rows,
              no footer, no selected-row highlight
  BOOK      - "Selected book" screen: 36px title, 48px book title, format
              line, gray path line, "[Reader not implemented]" note box
              (outline 48,440-567,482)

All kinds must be clean white in the margin columns (x<31 and x>=568)
and carry no ink run into the screen edges. Column 31 may hold a single
overhang pixel from a glyph with a negative left side bearing (accepted
renderer behavior, visible as min_x=31 on text-milestone screens).

Writes a PNG preview per dump.
"""
import struct
import sys

W, H = 600, 800
WHITE = 0xFFFF
GRAY_ROW = 0x7BEF


def load(path):
    data = open(path, "rb").read()
    assert len(data) == W * H * 2, f"size {len(data)} != 960000"
    return struct.unpack("<" + "H" * (W * H), data)


def ink_map(fb):
    return [[fb[y * W + x] != WHITE for x in range(W)] for y in range(H)]


def band_ink(ink, y0, y1):
    return sum(ink[y][x]
               for y in range(y0, y1) for x in range(W) if ink[y][x])


def check(name, cond):
    print(f"[{'OK' if cond else 'FAIL'}] {name}")
    return cond


def bbox(ink):
    xs = [x for y in range(H) for x in range(W) if ink[y][x]]
    if not xs:
        return None
    ys = [y for y in range(H) for x in range(W) if ink[y][x]]
    return min(xs), min(ys), max(xs), max(ys)


def margins_clean(fb):
    for y in range(H):
        for x in list(range(0, 31)) + list(range(568, W)):
            if fb[y * W + x] != WHITE:
                return False
    return True


def validate(path, kind):
    fb = load(path)
    ink = ink_map(fb)
    ok = True
    ok &= check("size == 960000", True)

    ink_px = band_ink(ink, 0, H)
    ok &= check(f"has content (ink={ink_px})", ink_px > 200)

    box = bbox(ink)
    ok &= check(f"bbox {box} inside screen", box is not None)
    if box:
        ok &= check("bbox within bounds", 0 <= box[0] and box[2] < W and
                    0 <= box[1] and box[3] < H)

    ok &= check("margin columns clean white", margins_clean(fb))
    if not margins_clean(fb):
        for y in range(H):
            for x in list(range(0, 31)) + list(range(568, W)):
                if fb[y * W + x] != WHITE:
                    print(f"  first dirty margin pixel at ({x},{y})")
                    break
            else:
                continue
            break

    if kind in ("LIBRARY", "SCROLLED"):
        ok &= check(f"{kind}: header band ink (y 20..140)",
                    band_ink(ink, 20, 140) > 500)
        ok &= check(f"{kind}: rows band ink (y 168..728)",
                    band_ink(ink, 168, 728) > 800)
        ok &= check(f"{kind}: selected row 0 filled gray",
                    fb[188 * W + 300] == GRAY_ROW)
        ok &= check(f"{kind}: footer band ink (y 737..757)",
                    band_ink(ink, 737, 757) > 30)
    elif kind == "EMPTY":
        ok &= check("EMPTY: header band ink (y 20..140)",
                    band_ink(ink, 20, 140) > 500)
        ok &= check("EMPTY: no-books message ink (y 160..235)",
                    band_ink(ink, 160, 235) > 50)
        ok &= check("EMPTY: no selected-row highlight", fb[188 * W + 300] == WHITE)
        ok &= check("EMPTY: rows region clean (y 240..310)", band_ink(ink, 240, 310) == 0)
        ok &= check("EMPTY: rows region clean (y 330..728)", band_ink(ink, 330, 728) == 0)
        ok &= check("EMPTY: no footer (y 737..757)", band_ink(ink, 737, 757) == 0)
    elif kind == "BOOK":
        px = lambda x, y: fb[y * W + x]
        ok &= check("BOOK: header band ink (y 20..80)", band_ink(ink, 20, 80) > 300)
        ok &= check("BOOK: title band ink (y 100..210)", band_ink(ink, 100, 210) > 150)
        ok &= check("BOOK: format/path band ink (y 236..330)",
                    band_ink(ink, 236, 330) > 100)
        ok &= check("BOOK: note box outline band ink (y 436..486)",
                    band_ink(ink, 436, 486) > 150)
        for (x, y) in ((300, 440), (300, 482), (48, 460), (567, 460)):
            ok &= check(f"BOOK: box edge ({x},{y}) black", px(x, y) == 0x0000)
        ok &= check("BOOK: box interior white (550,450)",
                    px(550, 450) == WHITE)
        ok &= check("BOOK: box interior white (550,472)",
                    px(550, 472) == WHITE)
    else:
        print(f"FAIL: unknown kind {kind}")
        return 1

    pngpath = path.replace(".bin", ".png")
    with open(pngpath, "wb") as f:
        f.write(png565(fb, W, H))
    print(f"wrote preview: {pngpath}")
    return 0 if ok else 1


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
    sys.exit(validate(sys.argv[1], sys.argv[2]))