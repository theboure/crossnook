# Milestone: CREngine EPUB Rendering Spike

**Status: HARDWARE VALIDATION PASS** (real Nook Simple Touch). Tagged
`milestone/crengine-rendering-spike`. No Library↔CREngine integration, no
persistence, no boot/rootfs/kernel changes, no partial E-Ink refresh, no
menus or chrome.

## What this milestone proves

CrossNook can open a plain EPUB with **koreader/crengine** and render
readable pages into the existing 600×800 RGB565 framebuffer stack as a
fully static, non-PIE, ARM EABI5 soft-float binary:

```
  test.epub ──▶ crengine LVDocView (lay out 600x800)
                    └─▶ LVColorDrawBuf RGB565 (16bpp) external buffer
                              └─▶ src/platform/nook/display.c (sole fb0 owner)
                                        └─▶ /dev/graphics/fb0
```

CREngine never opens fb0 (verified structurally, see below). Rendering
reuses the existing RGB565 canvas — no conversion layer is needed:
`LVColorDrawBuf(600, 800, canvas, 16)` draws directly into the framebuffer
pixel format (RGB565 LE, row size 1200).

## crengine bundle (pinned, reproducible)

All third-party sources are pinned in `work/` (gitignored) with sha256
recorded in `toolchain/crengine/pin.txt`; the Docker image builds
everything from those archives — no random host libs:

| component | version/pin | checksum |
|---|---|---|
| crengine | koreader/crengine @ `b05cf007` (master-tracked, no tags) | `7deeb72a…747c14` |
| zlib | 1.3.1 | `9a93b2b7…72df23` |
| xxHash | 0.8.2 | `baee0c6a…a79c4` |
| FreeType | 2.13.3 (existing pinned image dep) | `05503506…f063289` |

**License:** crengine is GPL-2.0 (per its README: "All source codes
(except thirdparty directory)… GNU GPL license, version 2"). Bundling
`libcrengine.a` statically into CrossNook carries GPL-2.0 obligations; this
spike only builds and tests it in test harnesses and does not yet merge it
into the product binary path.

## Build configuration that makes it work on musl

- **`-include stdint.h`** (`-std=c++17`): crengine's strict C++ sources use
  `uint8_t` etc. without guaranteeing `<stdint.h>` is in scope; force-
  including it (as the prototype found) fixes the build. musl's C headers
  don't implicitly pull it in.
- `crsetup.h` (generated, committed under `toolchain/crengine/crsetup.h`):
  `USE_ZLIB 1`, `USE_FREETYPE 1`, everything else 0 (no image decoders, no
  HARFBUZZ/BROTLI/TURBOJPEG), `COLOR_BACKBUFFER 1`, **`CR_USE_THREADS 0`**
  (→ no libpthread dependency), `DISABLE_LFS 1`.
- Toolchain: `arm-linux-musleabi-g++` (GCC 11.2.1) static, `-no-pie`,
  `-static`; links `libcrengine.a + libfreetype.a + libz.a + libxxhash.a`.
- No C++20 features; no pthread; koreader CRE sources listed in
  `toolchain/Dockerfile` compile as 38 objects into `libcrengine.a`.

## Testapp (`testapp/crossnook-cre-test`)

Usage:

```
crossnook-cre-test <epub>                     device mode (real Nook)
crossnook-cre-test --smoke   <epub>           host smoke assertions
crossnook-cre-test --dump DIR <epub>          host page dumps + dump.json
```

- Registers a font first (required: CREngine segfaults rendering a default
  document before a font is registered). `--font PATH` or default
  `/tmp/test-font.ttf` / `/opt/test-font.ttf`.
- Embedded stylesheet = `cr3.css` + `epub.css` + `html5.css` from the
  pinned crengine sources (byte-exact LF-normalized; generates
  `testapp/cre-fixtures/cre_css.h`, regenerable with
  `embed-css.py`). Fixture `test.epub` is deterministic (no DRM, Latin +
  Cyrillic, headings/paragraphs), sha256 `7824ae76…ba67`.
- Diagnostic contract: `CRE open OK`, `document loaded: N pages`,
  `layout size=600x800`, `page=P` per render; every failure prints
  `CRE ERROR: …` and exits non-zero (screen never left blank).
- Device input loop uses the platform semantic layer (`cn_input_*`,
  `src/platform/nook/input.h`): PAGE_NEXT/PAGE_PREV page; BACK/HOME or a
  power press exits cleanly. No touch actions.

## Host validation (qemu-arm, no Nook)

`bash testapp/build-cre.sh`:

1. cross-compiles the static non-PIE binary (no INTERP/Dynamic sections);
2. **structural fb0 check**: only `src/platform/nook/display.c` contains
   the `"/dev/graphics/fb0"` literal (CREngine stays out of the device);
3. `--smoke`: 79 pages, `layout size=600x800`, page 0/1 ink non-zero,
   page 1 differs from page 0 (NEXT != PREV), rendering page 0 again after
   PREV is byte-identical, and 64-byte canary guards around the render
   buffer prove no out-of-bounds writes;
4. `--dump`: writes pages 0..15 (`cre-dump/page-NNN.bin`, gitignored),
   `validate-cre.py` then re-checks dims/row-size, page count ≥ 2, each
   page's FNV checksum against `dump.json`, non-blank ink, byte-inequality
   page 0 vs 1, and writes PNG previews.

Current render (font = `testapp/test-font.ttf`, default crengine
font size): page 0 is a title strip (ink bbox y∈[6,70]), body pages span
y∈[6,780+] with ink counts around 100–111k pixels — genuine multi-page
book rendering including Cyrillic. My model build cannot display images, so
the visual layer was verified numerically (bbox/ink/checksum/determinism)
rather than by eyeballing the PNG previews.

Determinism: identical page checksums across consecutive runs
(e.g. page 0 `a08ad3cc`, page 1 `968fdf63`).

## Known behaviors / limitations

- Page count is only valid after the first `Draw()` (layout pass) —
  the app primes `goToPage(0)+Draw` before reading `getPageCount()`.
- static binary ≈ 13.1 MB (crengine + FreeType + zlib + xxHash).
- No embedded-image decoding in `crsetup.h` (books with images render
  text; image placeholders may show as missing objects). Fine for the
  spike; re-enable codecs later if desired.
- Device mode (fb0 + input loop) is compiled but only runnable on hardware.
- `test.epub` is 79 pages at the current font size — deliberately
  larger than the "at least 2 pages" spike minimum.

## On-device hardware validation (real Nook Simple Touch)

Confirmed on hardware:

- the static ARM CREngine executable starts successfully;
- the test EPUB opens and lays out successfully at 600×800;
- the first page and body pages render correctly;
- Latin and Cyrillic text render correctly;
- paragraph wrapping/layout is sane;
- PAGE_NEXT / PAGE_PREV navigation works reliably, and repeated
  navigation is stable;
- BACK / HOME / long-POWER exit cleanly as implemented;
- no framebuffer corruption, crash, or obvious memory-related failure was
  observed.

**Performance observation (recorded, not measured):** input/page
diagnostics appear immediately in the console; the visible page transition
on the physical E-Ink display is generally fast, occasionally around
~0.5 s end-to-end. This is NOT attributed specifically to CREngine or to
E-Ink — there was no explicit timing instrumentation on-device. No
performance optimization is required for this milestone.

## On-device validation procedure (recorded for repetition)

1. Push `crossnook-cre-test <epub>` + `test.epub` + `test-font.ttf` to
   `/tmp` on the Nook; run, confirm `CRE open OK`, `document loaded`,
   `layout size=600x800`, readable page on screen.
2. PAGE_NEXT / PAGE_PREV flip pages; BACK / HOME / power exit cleanly.
3. Confirm the screen is never left blank on any error path.

These steps are what the milestone's HARDWARE VALIDATION PASS above
records.

## Files

- `toolchain/Dockerfile` — pinned zlib/xxhash/crengine + in-image RGB565
  smoke (prints `IMG CRE RGB565 OK`)
- `toolchain/crengine/pin.txt`, `crsetup.h`, `proto-build.sh`,
  `cresmoke.sh`
- `testapp/crossnook-cre-test.cpp`, `build-cre.sh`, `validate-cre.py`
- `testapp/cre-fixtures/` — `test.epub`, css sources, `embed-css.py`,
  `make-fixtures.sh`, generated `cre_css.h`
- `docs/` — this milestone

Reference: prototype (throwaway containers) rebuilt the exact recipe —
all 38 CRE_SRCS objects, `-include stdint.h`, font registration required —
committed as `toolchain/crengine/proto-build.sh`.