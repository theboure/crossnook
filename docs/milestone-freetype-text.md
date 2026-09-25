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
- Render: size 960000, checksum `e45d6ba9`, ink 37754 px,
  bbox (31,25)-(546,751) — inside screen.
  - not empty, not all-white, not all-black;
  - Latin and Cyrillic lines present;
  - each size sample renders the full phrase via multiline wrap
    (18px = 1, 24/32px = 2, 48px = 3 wrapped lines);
  - bottom at y=751 (49px margin, fully visible);
  - preview `testapp/fb-text.png` shows a legible title, Latin and
    Cyrillic lines, and the wrapped size samples.

## On-device validation (PASS — real Nook Simple Touch)

Deployed to `/tmp`, run as `/tmp/crossnook-text /tmp/test-font.ttf`.
Confirmed on device:

- FreeType initializes and renders correctly;
- Latin text is legible;
- Cyrillic text is legible;
- anti-aliased glyphs render correctly;
- multiple font sizes are visibly different;
- framebuffer refresh works.

Two layout defects found on-device and fixed (both layout-only; the
FreeType rendering path, RGB565 blending, metrics and fb write are
unchanged):

1. **Bottom clipping** — the word-wrap code force-broke every short block
   at its last space, doubling the vertical extent and clipping the last
   sample past y=800. Wrap now happens only on real width overflow
   (`i < n`); line-spacing factor tightened 6/5 → 11/10.
2. **Apparent horizontal truncation of the size samples** — the
   24/32/48 px samples showed only a few words with no continuation line.
   Root cause: NOT a renderer clip; the sample strings were deliberately
   shortened by design (`"The quick brown fox jumps"` / `"The quick brown
   fox"`), so each rendered as a single complete line. Fixed by rendering
   the SAME full phrase at all four sizes through `render_block()`, which
   now wraps them (24px: 2 lines, 32px: 2, 48px: 3; 18px: 1), replacing
   per-sample `SIZE N` labels with a legend line, and adding host
   validation asserting the expected wrapped-line count, ink floor, and
   right-edge extent per size.

Re-validated host-side (`build-text.sh` + `validate-text.py` all green),
deploy repeated.

## Post-tag layout-correction visual revalidation (2026-09-25)

The post-tag layout correction was visually revalidated on the physical Nook
Simple Touch using the current published commit
`8161ad081af31cef8093f66a6c164bfb0a9095ce`.

Validated inputs:

- `crossnook-text` SHA-256:
  `4750d503224df2e0b2187217b7c052e5e991af5367f275e3144819b25f941269`;
- `test-font.ttf` SHA-256:
  `7da195a74c55bef988d0d48f9508bd5d849425c1770dba5d7bfc6ce9ed848954`.

Target execution reported `crossnook-text: rendered 600x800 to
/dev/graphics/fb0 (960000 bytes), sleeping`. The framebuffer SHA-256 was
`d7ae01851d068916e937394337127c37e92f9dba01f8a567b53aa526c8330e13` before
the visible render and
`4ba45d9c36a222903b18918dbfffefae0997640891228db3a2a272ec6` while the human
was observing the rendered text. These hashes are supporting framebuffer
evidence only; they do not establish visual behavior by themselves.

The human confirmed on the physical panel:

- `CrossNook` and `DejaVu Sans` were visible;
- the Cyrillic pangram was visible and legible;
- the 18 / 24 / 32 / 48 px samples were visible;
- the 48 px sample fit completely on the physical display;
- the large sample had no right- or bottom-clipping; and
- the physical E-Ink panel actually repainted.

Therefore: **target execution: PASS; visual physical: PASS; post-tag layout
correction: visually confirmed.**

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
