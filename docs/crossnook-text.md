# Milestone: FreeType text rendering

**Status: host-validated (qemu-arm + pixel checks); on-device PASS pending.**
Gate: do not mark complete until the real Nook shows the screen.

Single static native ARM diagnostic `crossnook-text` that loads a TTF font
at runtime and renders high-quality anti-aliased UTF-8 text through
FreeType into the proven RGB565 framebuffer (`/dev/graphics/fb0`).
Reuses: pinned musl/EABI toolchain, 600x800 RGB565 software framebuffer,
stride-1200 full-frame write (lseek 0 + 960000 bytes), host validation via
qemu-arm inside Docker.

Explicitly out of scope (NOT built here): EPUB, CREngine, CSS,
hyphenation, font-settings UI, library scanning, partial E-Ink refresh,
boot image / ramdisk / rootfs changes.

## Deliverables

| file | purpose |
| --- | --- |
| `testapp/crossnook-text.c` | app: FreeType + UTF-8 + RGB565 renderer |
| `testapp/build-text.sh` | cross-compile + host smoke/render validation |
| `testapp/validate-text.py` | host pixel checks + PNG preview |
| `testapp/test-font.ttf` | DejaVu Sans 2.37 (redistributable, see below) |
| `testapp/test-font-LICENSE.txt` | the font's license text |
| `toolchain/Dockerfile` | adds pinned FreeType 2.13.3 static cross lib |
| `work/freetype/freetype-2.13.3.tar.xz` | pinned FreeType source (local, ignored by git) |

## Toolchain: FreeType added reproducibly

The pinned image now also cross-compiles FreeType **2.13.3**
(sha256 `0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289`)
as `libfreetype.a` (static, ARM EABI5 soft-float) installed under
`/opt/freetype`, with all optional deps disabled
(`--without-zlib/bzip2/png/harfbuzz/brotli`) — a TTF renderer needs none
of them. The Dockerfile re-verifies the source sha256 and cross-links a
tiny FT test program as a build-time sanity gate.

To rebuild the image after the change: `bash toolchain/build-smoke.sh`
(the toolchain smoke test is unchanged).

## Test font

`test-font.ttf` is **DejaVu Sans 2.37** (from
`dejavu-fonts-ttf-2.37.zip`, archive sha256
`7576310b219e04159d35ff61dd4a4ec4cdba4f35c00e002a136f00e96a908b0a`).
Its license (included as `test-font-LICENSE.txt`) is the Bitstream Vera
license family and explicitly permits copying, redistribution and
sublicensing as long as the license text ships with the font — which is
why committing the `.ttf` is allowed here. File sha256:
`7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954`.

To swap in another font, replace `testapp/test-font.ttf` (Cyrillic +
Latin coverage) and re-run `build-text.sh`.

## Application behavior

`crossnook-text <font.ttf>` (device mode): opens `/dev/graphics/fb0`,
reads the font, renders one screen, writes the full frame, then idles.
Host modes: `--smoke <font>` (FreeType init + glyph coverage check);
`--render <font> <out.bin>` (render and dump the raw 960000-byte frame).

Implemented per spec:

- **UTF-8 decoding** to codepoints (1..4-byte sequences validated:
  overlong, surrogate, >U+10FFFF and bad-continuation rejected); the text
  loop iterates codepoints, never raw bytes.
- **Anti-aliased glyphs alpha-blended** in RGB565 (fore/back channels
  blended by the glyph's 8-bit coverage — no black/white thresholding).
- **baseline positioning** via size metrics ascender;
- **glyph advance** from `slot->advance`, hinted identically for
  measurement and rendering;
- **multiline + word wrap** (breaks after the last space that fits);
- **screen clipping** of every glyph bitmap to 600x800;
- **left/right margins** (32 px) internal constants;
- **line spacing** internal factor (11/10 of the font's line height);
- **negative bearings / offsets** handled via `bitmap_left/bitmap_top`.

Kerning is not implemented (allowed to be skipped; kept out to avoid
scope creep). The diagnostic screen shows (white bg, black text):

```
CrossNook                          (48 px title)
DejaVu Sans                        (14 px caption)
The quick brown fox jumps over the lazy dog.     (24 px)
Съешь ещё этих мягких французских булок, да выпей чаю.  (24 px, wraps)
SIZE 18 …  SIZE 24 …  SIZE 32 …  SIZE 48 …      (18/24/32/48 px samples)
```

## Build + host validation

```bash
bash testapp/build-text.sh        # build + metadata + smoke + render
python testapp/validate-text.py testapp/fb-text.bin
```

Observed host result (qemu-arm, deterministic):

```
SMOKE font=test-font.ttf family=DejaVu Sans
SMOKE latin:    36 chars, 0 missing, 0 bad_utf8 --> OK
SMOKE cyrillic: 46 chars, 0 missing, 0 bad_utf8 --> OK
SMOKE OK
RENDER size=960000 checksum=07fe8e97 ink=26063 bbox=(31,25)-(523,660)
```

`validate-text.py` asserts: file == 960000 bytes; has ink and background
(not empty/all-white/all-black); the Latin (y165-217) and Cyrillic
(y225-277) bands have ink; the four SIZE sample bands (18/24/32/48 px,
ending at y660) are present and differ measurably; the whole content
stays inside (0,0)-(600,800) with the 48px sample fully visible
(ink maxy < 700). It writes `testapp/fb-text.png` for visual inspection.
Works because the render is byte-deterministic for the fixed font +
layout.

### Layout-repair note (from the on-device PASS run)

On-device the 48px sample was clipped at the bottom. Root cause: the
word-wrap code broke every line at its last space even when the whole
line fit, so each short block ("SIZE 18", the 14px caption, ...) was
force-emitted as two lines and every block downstream shifted down ~2x,
pushing the last sample past y=800. Fixed by wrapping only on an actual
width overflow (`i < n`) and tightening the internal line-spacing factor
(6/5 -> 11/10) and inter-section gaps. The FreeType rendering path,
blending, metrics and fb write are unchanged; the render re-validated
with the 48px sample bottom at y=660 (140px margin).

## Deploy to the Nook

Use `/tmp`, not `/data` (the diagnostic NookManager image's `/data` is
internal eMMC):

```bash
adb push testapp/crossnook-text /tmp/crossnook-text
adb push testapp/test-font.ttf /tmp/test-font.ttf
adb shell chmod 755 /tmp/crossnook-text
adb shell /tmp/crossnook-text /tmp/test-font.ttf
```

Expected: a white screen with black text (title, Latin, Cyrillic, four
sizes) as above. Remove afterwards:

```bash
adb shell rm /tmp/crossnook-text /tmp/test-font.ttf
```

## Non-goals

No EPUB/CREngine/CSS/hyphenation/font-UI/library scan/partial E-Ink
refresh; no changes to boot image, ramdisk, rootfs, boot blobs, input
stack or framebuffer architecture.