# Milestone: Library → CREngine Reader Integration

**Status: HARDWARE VALIDATION PASS** (real Nook Simple Touch). Tagged
`milestone/library-reader-integration`. No persistence, settings, TOC, or
sync. The Library UI opens real EPUBs through a reusable reader layer backed
by CREngine: HOME → LIBRARY → select EPUB → reader (page 0) → NEXT/PREV
flips CREngine pages → BACK returns to the Library with the selection and
scroll position intact. FB2/TXT and broken-EPUB activations deterministically
fall back to the SELECTED_BOOK screen; long-POWER exits cleanly from any
state.

## What this milestone proves

The Library↔CREngine gap is closed WITHOUT changing the module contracts:

```
library rows (library.c) ──▶ ui.c activation
        ──▶ src/reader/reader.{h,cpp}   (cn_reader_* C API; owns CREngine)
              └─▶ LVDocView renders 600x800 RGB565 straight into the canvas
                        └─▶ display.c (sole fb0 owner) ─▶ /dev/graphics/fb0
```

- CREngine is confined to `src/reader/reader.cpp`; nothing else in `src/`
  includes crengine headers, and the fb0 literal structurally belongs to
  `display.c` alone (verified by every build script).
- `ui.c` remains plain C — the reader is reached through a pure C API
  (`cn_reader_*`, `cn_reader_config`) with zero crengine types leaking.
- Reader pages turn through the same semantic input events
  (`CN_INPUT_PAGE_NEXT/PREV`) used by the whole platform; touches are
  suppressed in the real reader (no diagnostic marker).

## Architecture

### `src/reader/reader.h` — C API (no CREngine types)

`CN_READER_W/H` (600/800); `cn_reader_config`:

```
font_path           TTF/OTF for CREngine (NULL = search /tmp, /opt)
font_size           px; 0 = engine default (24)
line_spacing_pct    0 or 100 = engine default
word_spacing_pct    RESERVED (pinned CREngine has no word-spacing setter)
margin_px           uniform page margin; 0 = engine default
alignment           RESERVED   fg_color/bg_color  RESERVED
dark_mode           RESERVED   focus_reading       RESERVED
```

API: `cn_reader_new(cfg)` (owns config; font is registered lazily,
deduplicated per path — several reader instances may share one font),
`cn_reader_free`, `cn_reader_apply_config` (controlled re-layout; future
layout-affecting changes must go through it, never direct CREngine pokes),
`cn_reader_open/path`, `cn_reader_close`, `cn_reader_is_open`,
`cn_reader_pages` (0 closed), `cn_reader_page`, `cn_reader_next/prev/go`
(clamped at first/last page), `cn_reader_render(r,rgb565,w,h)` (exact
600x800 only; -1 on closed/invalid).

Diagnostics (stderr): `READER open path=…`, `READER pages=N`,
`READER page=N` (only on change), `READER close`, `READER ERROR: …`.

### `src/reader/reader.cpp` — CREngine implementation

Reproduces the hardware-validated spike recipe: `InitFontManager("")`,
`RegisterFont`, `LVDocView(16)`, `setStyleSheet(cre_css, true)`,
`setViewMode(DVM_PAGES,1)`, `Resize(600,800)`, `LoadDocument`, then a
priming `goToPage(0)+Draw()` into an internal scratch so `getPageCount()`
is valid before the first real render. `apply_layout` honors font size
(`setFontSize`), interline percent (`setDefaultInterlineSpace`) and
uniform margins (`setPageMargins(lvRect)`). Pages are drawn with
`LVColorDrawBuf(600,800,target,16)` — RGB565, stride 1200, straight into
the caller's canvas.

Stylesheet: `src/reader/{cr3,epub,html5}.css` embedded at build time into
`src/reader/cre_css.h` by `embed-css.py` (deterministic; moved here from
`testapp/cre-fixtures/` with `git mv` so the reader owns its CSS).

## UI integration (`src/ui/ui.{c,h}`)

- New state `CN_UI_READER` appended to the END of the enum (existing enum
  values unchanged → older binaries stay valid); maps to name "READER"
  exactly like `CN_UI_READER_TEST` (mutually exclusive states).
- `cn_ui_set_reader(ui, cfg)`: attaches/detaches a reader owned by the UI.
  Without an attached reader, EPUB activation falls back to SELECTED_BOOK
  — preserving the behavior of all pre-reader binaries/regressions.
- Activation: EPUB + reader attached → `cn_reader_open`; on open failure
  (genuinely broken EPUB, missing file) → deterministic SELECTED_BOOK,
  never a blank screen or crash. FB2/TXT always → SELECTED_BOOK.
- `cn_ui_reader_pages`, `cn_ui_reader_page`; render via
  `cn_reader_render` into the canvas. FREE of the reader happens in
  `cn_ui_free` / `cn_ui_set_reader`.

## Testapps and fixtures

- `src/app/reader-test.c` → `testapp/crossnook-reader-test`
  (device mode; `--smoke <font> <dir>`; `--dump <font> <dir> <outdir>`).
  The smoke drives the full UI state machine (T1–T13): HOME→LIBRARY,
  EPUB→READER page 0, page-0 ink + checksum, NEXT changes page, PREV
  restores page 0 bit-for-bit, first/last clamps, BACK preserves
  selection/top, open-A → HOME → open-B (reopen always lands on page 0),
  invalid EPUB → SELECTED_BOOK fallback, FB2/TXT never enter the reader,
  direct-API OOB canary + invalid-open + NULL/wrong-size render refusal +
  `cn_reader_apply_config` re-layout (font_size 40) + long-POWER clean exit.
  The dump writes `home, library, reader_p0, reader_p1, reader_last,
  invalid, fb2` frames + `dump.json` (FNV-1a checksums).
- `testapp/reader-fixtures/` (regenerated deterministically by
  `make-fixtures.sh`): `valid.epub` + `valid2.epub` (= `test.epub`,
  sha256 `7824ae76…ba67`), `broken.epub` — a zip containing only
  `mimetype`, so `LoadDocument` genuinely fails (plain text is silently
  parsed as a 1-page txt book, which is why the fixture is a zip),
  `book.fb2`, `notes.txt`. Sort order by title: book(0), broken(1),
  notes(2), valid(3), valid2(4).
- `testapp/build-reader.sh` builds CREngine regression first (fresh
  baseline), regenerates fixtures + embedded CSS, cross-compiles
  `crossnook-reader-test`, runs smoke + dump under qemu-arm, then host-
  validates with `validate-reader.py` (also verifies fb0 ownership) and
  prints artifact checksums.
- `testapp/crossnook-cre-test.cpp` now consumes the same `cn_reader`
  layer (no crengine includes) — the CRE regression and the reader share
  one rendering code path, and the reader is regression-checked against
  it. `build-ui.sh` / `build-library.sh` link `reader.cpp` too (the UI
  module owns the reader); their tests still exercise the no-reader
  fallback paths.

## Host validation (qemu-arm, no Nook needed)

`bash testapp/build-reader.sh` (plus the individual regressions):

- `crossnook-cre-test` — CRE SMOKE OK, CRE VALIDATE OK (79 pages,
  deterministic in-process page=0-back==page-0).
- `crossnook-reader-test --smoke` — `SMOKE … bad=0 -> OK` covering all
  T1–T13 scenarios, including direct-API guards.
- `--dump` + `validate-reader.py` — READER VALIDATE OK: dims 600x800,
  all 7 frames present with matching checksums and ink>0, reader_p0 !=
  reader_p1, reader_last != reader_p0, invalid/fb2 distinct SELECTED
  frames, reader page count == CRE page count (79), and page-0/page-1
  ink within 5% of the CRE regression baseline (reported pixel-identical
  on a representative run).
- All prior regressions green: `build-test.sh`, `build-text.sh`,
  `build-ui.sh`, `build-library.sh`.

**Nondeterminism note (contract, not a bug in our code):** the standalone
CREngine build renders a small number of text-run glyphs via a fallback
font chosen from an unordered internal collection, so page bitmaps are not
byte-stable ACROSS builds of even this repo's own CRE regression (same
sources/flags compile to identical binaries; same binary reproduces
byte-identical dumps). Pixel-identity is therefore validated in-process
and compared against the CRE baseline with a tolerance; byte-for-byte
equality is informational. The durable byte-level gate belongs to an
on-device golden capture (next milestone), which is also where the visual
quality can be judged.

## Known behaviors / limitations

- No book progress persistence, TOC, settings UI, images, or partial E-Ink
  refresh — all out of scope for this milestone.
- `word_spacing_pct`, `alignment`, color/dark-mode and `focus_reading`
  are reserved but not yet applied; `line_spacing_pct`, `margin_px`,
  `font_size` are honored via `cn_reader_apply_config`.
- CREngine's own parse fallback makes a plain-text `.epub` become a
  1-page txt book; only structurally broken archives fail `open` (the
  fixture is engineered accordingly).
- Static binaries remain ~13 MB (CREngine + FreeType + zlib + xxHash).

## On-device hardware validation (real Nook Simple Touch)

Confirmed on hardware:

- HOME → Library works;
- a valid EPUB enters the real UI_READER;
- CREngine page 0 renders correctly;
- PAGE_NEXT / PAGE_PREV work;
- BACK closes the Reader and restores the same Library selection and
  viewport;
- HOME closes the Reader and returns HOME;
- repeated open/close cycles are stable;
- an invalid EPUB falls back safely and never leaves a blank screen;
- FB2/TXT do not enter the EPUB Reader path;
- long POWER exits cleanly;
- no framebuffer corruption or crashes observed.

## On-device validation procedure (recorded for repetition)

1. Push `crossnook-reader-test` + `reader-fixtures/` + `test-font.ttf`
   to `/tmp`; run current device mode with font, books-dir, and an explicit
   writable state-dir argument (for example `/tmp/crossnook-state`).
2. HOME → LIBRARY → open `valid.epub` → confirm page 0 renders; PAGE_NEXT/
   PREV turns pages; BACK returns to the Library preserving selection/
   scroll; verify `broken.epub` shows SELECTED_BOOK (never blank);
   long-POWER exits.
3. Compare visible pages to the host-side PNG previews
   (`testapp/reader-dump/frame-*.png`) and the CRE regression pages.

These steps are what the milestone's HARDWARE VALIDATION PASS above
records.

## Files

- `src/reader/reader.h`, `src/reader/reader.cpp` — reader layer (C API +
  CREngine implementation); `src/reader/*.css`, `embed-css.py`,
  `cre_css.h` (moved from `testapp/cre-fixtures/` via `git mv`)
- `src/ui/ui.c`, `src/ui/ui.h` — `CN_UI_READER`, reader attach, page-turn,
  activation fallbacks
- `src/app/reader-test.c` — integration test app
- `testapp/build-reader.sh`, `testapp/validate-reader.py`,
  `testapp/reader-fixtures/` (`valid.epub`, `valid2.epub`, `broken.epub`,
  `book.fb2`, `notes.txt`, `make-fixtures.sh`)
- `testapp/crossnook-cre-test.cpp`, `build-cre.sh` — CRE regression moved
  onto the shared reader layer
- `testapp/build-ui.sh`, `testapp/build-library.sh` — now link the reader
- `docs/` — this milestone
