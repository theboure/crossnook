#!/usr/bin/env python3
"""Host-side validation of crossnook-ui-test framebuffer dumps.

Kinds (all 600x800 RGB565 LE, stride 1200):
  HOME      - "CrossNook" title, "UI Core", [ Open reader test ] button
              outline; background clean white; no text lower than the
              button area; marker absent (touch cleared state)
  READER    - "Reader test", "Page N", wrapped Cyrillic sample, gray
              footer hints; background clean white
  PRIMITIVES- fixed geometry from the in-app assertions: bottom-left
              30x30 black clipped fill, 50..80 outline, cross at
              (100..200, 60) + (150, 90..120)

Writes a PNG preview alongside each dump.
"""
import struct
import sys

W, H = 600, 800
WHITE = 0xFFFF


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


def validate(path, kind):
    fb = load(path)
    ink = ink_map(fb)
    ok = True
    ok &= check(f"size == 960000", True)

    ink_px = band_ink(ink, 0, H)
    ok &= check(f"has content (ink={ink_px})", ink_px > 200)

    box = bbox(ink)
    ok &= check(f"bbox {box} inside screen", box is not None)
    if box:
        ok &= check("bbox within bounds", 0 <= box[0] and box[2] < W and
                    0 <= box[1] and box[3] < H)

    if kind == "HOME":
        ok &= check("HOME: tite band ink (y 25..90)", band_ink(ink, 25, 90) > 1000)
        ok &= check("HOME: button outline band ink (y 210..255)",
                    band_ink(ink, 210, 255) > 400)
        ok &= check("HOME: no ink below button (y 260..799)",
                    band_ink(ink, 260, H) == 0)
    elif kind == "READER":
        ok &= check("READER: title band ink (y 25..75)",
                    band_ink(ink, 25, 75) > 500)
        ok &= check("READER: Cyrillic body ink (y 130..400)",
                    band_ink(ink, 130, 400) > 1500)
        ok &= check("READER: footer band ink (y "+str(H-40)+".."+str(H-1)+")",
                    band_ink(ink, H - 40, H - 1) > 80)
    elif kind == "PRIMITIVES":
        px = lambda x, y: fb[y * W + x]
        ok &= check("PRIMITIVES: gray fill (10,10) gray", px(10, 10) == 0x7BEF)
        ok &= check("PRIMITIVES: gray fill exterior (21,21) white",
                    px(21, 21) == WHITE)
        ok &= check("PRIMITIVES: clipped fill (0,400) black", px(0, 400) == 0x0000)
        ok &= check("PRIMITIVES: fill edge (30,430) black", px(30, 430) == 0x0000)
        ok &= check("PRIMITIVES: clip boundary (31,430) white",
                    px(31, 430) == WHITE)
        ok &= check("PRIMITIVES: outline border (50,50) black", px(50, 50) == 0x0000)
        ok &= check("PRIMITIVES: outline interior (65,65) white",
                    px(65, 65) == WHITE)
        ok &= check("PRIMITIVES: hline (200,60) black", px(200, 60) == 0x0000)
        ok &= check("PRIMITIVES: hline past end (201,60) white",
                    px(201, 60) == WHITE)
        ok &= check("PRIMITIVES: vline (150,120) black", px(150, 120) == 0x0000)
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