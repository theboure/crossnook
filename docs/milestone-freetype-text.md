# Milestone: FreeType text rendering

**Status: HARDWARE VALIDATION PASS** — confirmed on the physical Nook
Simple Touch (NookManager diagnostic image, `/tmp` deploy).

This milestone adds FreeType-based anti-aliased UTF-8 text rendering to
the proven framebuffer environment, as a standalone native ARM diagnostic
(`crossnook-text`) deployed over ADB. See `docs/crossnook-text.md` for
build/deploy/validation details.

## Scope (what this milestone is)

1. Add FreeType to the pinned cross toolchain reproducibly (static
   `libfreetype.a`, free of external deps).
2. `crossnook-text`: load a TTF at runtime, decode UTF-8 to codepoints,
   alpha-blend grayscale FreeType coverage into the existing RGB565
   software framebuffer, and push the 960000-byte frame via the proven
   lseek+write path.
3. Host-side validation without a device:
   - FreeType init + Latin + Cyrillic glyph coverage (`--smoke`);
   - deterministic full-screen render dumped to `fb-text.bin` plus pixel
     assertions and a PNG preview (`--render` + `validate-text.py`).

## Host validation results (this build)

- ELF: static EXEC, ARM EABI5, soft-float (Flags 0x5000200), v5T,
  no INTERP/Dynamic.
- Smoke: `SMOKE OK`; latin 36 chars / 0 missing; cyrillic 46 chars /
  0 missing.
- Render: size 960000, checksum `07fe8e97`, ink 26063 px,
  bbox (31,25)-(523,660) — inside screen.
  - not empty, not all-white, not all-black;
  - Latin and Cyrillic lines present;
  - the four size bands measurably differ;
  - 48px sample bottom at y=660 (140px margin, fully visible);
  - preview `testapp/fb-text.png` shows a legible title, Latin and
    Cyrillic lines and four sizes.

## On-device validation (PASS — real Nook Simple Touch)

Deployed to `/tmp`, run as `/tmp/crossnook-text /tmp/test-font.ttf`.
Confirmed on device:

- FreeType initializes and renders correctly;
- Latin text is legible;
- Cyrillic text is legible;
- anti-aliased glyphs render correctly;
- multiple font sizes are visibly different;
- framebuffer refresh works.

One layout defect found on-device and fixed (no rendering-architecture
changes): the word-wrap code force-broke every short block at its last
space, doubling the vertical extent and clipping the 48px sample past
y=800. Wrap now happens only on real width overflow; line-spacing factor
tightened 6/5 -> 11/10; after the fix the 48px sample ends at y=660.
Re-validated host-side, deploy repeated.

## Encountered during implementation

- FreeType's `configure` needs BOTH a native host compiler (`gcc` +
  `libc6-dev` in the image for its `apinames` build-tool) and the
  cross compiler — added to the pinned image.
- The linker sanity check checks the produced binary, not the archive
  (`file` on the `.a` only prints "current ar archive").
- Deterministic render (same font + layout) means host assertions can use
  fixed y-band ranges for the Latin/Cyrillic/size lines.
- Baseline instrumentation (a `getenv("CNT_DEBUG")`-gated print in
  `render_block`) made the false-wrap bug (every block emitted 2 lines)
  obvious; the prints are removed from the final code.

## Non-goals (unchanged)

No EPUB, CREngine, CSS, hyphenation, font-settings UI, library scanning,
partial E-Ink refresh, rootfs/boot image changes, or changes to the
input stack / framebuffer architecture.