# Milestone: Library Core (filesystem book library + library UI)

**Status: HARDWARE VALIDATION PASS** (real Nook Simple Touch). Screens,
navigation, touch selection, the Selected Book screen, the empty state and
the long-POWER exit all confirmed on-device. Tagged `milestone/library-core`.

## Scope (what this milestone is)

Add the first real content to CrossNook: a non-recursive filesystem book
scanner and a library-list UI with highlight, paging, touch selection and
a "Selected book" diagnostic screen, composed on the validated module tree:

```
src/library/library.{c,h}     scan/add/sort/query (no parsing)
src/ui/ui.c                   + LIBRARY / SELECTED_BOOK states
src/app/ui-library-test.c     executable + device/host modes
```

New executable `crossnook-library-test <font.ttf> <books-dir>` (device mode
uses `/tmp/books` in deployment). `crossnook-test`, `crossnook-text`,
`crossnook-ui-test` remain as regression references — the ui-test HOME and
reader renders stay byte-identical (the new `[ Open library ]` button is
drawn only when a library is attached).

## Scanner (`src/library/library.c`)

- Non-recursive; `opendir`/`readdir`, child type raw (`DT_DIR`/`DT_REG`,
  falling back to `stat()` when unknown).
- Recognized formats, case-insensitive: `.epub`, `.fb2`, `.txt`
  (`cn_book_format_name()` → "EPUB"/"FB2"/"TXT").
- Ignored: hidden (leading `.`), directories, unsupported extensions, and
  temporary files (name contains `#`, ends with `~`, or ends with `.tmp`
  case-insensitive).
- Title = filename minus its last extension (e.g. "Война и мир.epub" ->
  "Война и мир").
- Deterministic sort: case-insensitive fold of ASCII plus Cyrillic
  (lowercase mapping applied per UTF-8 lead byte; letters A-z, А-я fold to
  their lowercase form, everything else compares by byte), ties broken by
  raw `strcmp(title)` then `strcmp(filename)`.
- Dynamic array (capacity starts at 8, doubles); allocation failures free
  partial results and return NULL (reported to stderr).
- No EPUB/FB2 parsing, no metadata, no signatures, no persistence.

## Library UI (`src/ui/ui.c`)

- New states appended (order preserved): `CN_UI_LIBRARY`,
  `CN_UI_SELECTED_BOOK`.
- Layout constants: row height `CN_UI_LIB_ROW_H 40`, first row
  `CN_UI_LIB_ROW_TOP 168`, footer baseline `CN_UI_LIB_FOOTER_Y 750`;
  14 rows fit. Selected row = gray `0x7BEF` fill `x 32..567` + black text;
  unselected rows black on white; title truncated with an ellipsis
  (`…` U+2026 via `cn_text_measure`, else `...`) when it cannot fit.
- Footer `"%d–%d of %d"` (en dash) on the 750 baseline, gray.
  Empty library renders the header + "No books found." and no footer.
- Interaction: PAGE_NEXT increments the selection and scrolls the viewport
  once the selection exits the last row (top clamped to `count - rows`);
  PAGE_PREV moves up and scrolls when the selection leaves the first row.
  A TOUCH_UP on a visible row selects it; touching the already-selected
  row opens SELECTED_BOOK. BACK returns to LIBRARY preserving selection and
  viewport. HOME from any state goes home. The HOME screen gains a second
  button "`[ Open library ]`" (y 300..342), rendered and hit-tested only
  when a library is attached.
- SELECTED_BOOK: 36 px "Selected book" header, 48 px title, format name,
  gray path (all ellipsized), and a `[Reader not implemented]` note box
  outlined across the content column (48..567, y 440..482).
- The input event stream (touch aggregation, POWER hold timing, MENU/BACK/
  HOME semantics) is unchanged from the UI Core milestone.

## Host validation results (this build)

- ELF: static EXEC, ARM EABI5, soft-float (Flags 0x5000200), no
  INTERP/Dynamic.
- Structural: `"/dev/graphics/fb0"` literal still appears only in
  `src/platform/nook/display.c`.
- Fixtures: `testapp/library-fixtures/` — 41 supported books (latin +
  cyrillic, mixed case, spaces, extensions) plus ignored extras
  (`notes.pdf`, `cover.JPG`, `overview.zip`, `note.TMP`, `draft.txt~`,
  `#lock#.txt`, `.hidden.epub`, `subdir/`). Regenerated deterministically
  by `make-fixtures.sh` (the `~` decoy intentionally matches a gitignore
  rule and is never committed).
- `--smoke` (qemu-arm): scan count == 41 (proves every ignore rule),
  deterministic sort probes `01 zone < 10 ten < test book < The Hobbit <
  Война и мир`, empty-dir scan == 0, UI transitions (enter library via
  touch, selection crossing into the next viewport, PAGE_PREV, row touch
  select/activate, BACK preserving sel/top, none of the checks fail), and
  an ellipsis right-edge probe that draws nothing past the content column.
- Dumps: `--dump-library` (sel=0 top=0), `--dump-scrolled` (14 NEXT presses
  -> sel=14 top=14), `--dump-empty`, `--dump-book` (select+activate row 0).
  All four validated pixel-exact: 960000 bytes, bbox in-bounds, margin
  columns clean (x < 31 and x >= 568 white; column 31 may hold a negative
  left-side-bearing overhang — inherited from the validated text renderer),
  per-kind bands (header/rows/footer, gray selected-row fill, empty-state
  body empty, selected-book box outline and interior).
- `host-live-library-test.sh` E2E: two scripted sessions under qemu-arm
  with FIFO input devices + a file framebuffer, exercising HOME ->
  LIBRARY via the touch button, paging (sel=14 top=14, sel=40 top=27,
  no-op after the last book), row selection + re-tap to SELECTED_BOOK,
  BACK preserving sel/top, HOME, long-POWER exit, and the empty-library
  session (books=0 stays in LIBRARY). All asserts green; final fb frames
  960000 bytes.
- Regressions: `build-test.sh`, `build-text.sh` and `build-ui.sh` still
  pass (text `e45d6ba9`; ui-test SMOKE/dumps/E2E unchanged).

## On-device validation results (real Nook Simple Touch)

1. HOME -> Library navigation works — PASS.
2. Latin and Cyrillic book titles render correctly — PASS.
3. NEXT/PREV selection and scrolling work — PASS.
4. Touch row selection and activation work — PASS.
5. Selected Book screen shows title / format / path — PASS.
6. BACK restores the same library selection and viewport — PASS.
7. HOME returns to the main screen — PASS.
8. Empty library shows "No books found." — PASS.
9. Long POWER (>= 2 s) exits cleanly — PASS.

Deploy: see `docs/crossnook-library-test.md` ("Deploy to the Nook").

## Touch marker (diagnostic only)

The black-box + white-dot touch marker from UI Core bring-up is a
diagnostic aid and is intentionally **not** rendered on the Library and
Selected Book screens; it remains available only on HOME and the Reader
Test screen (the UI Core diagnostic mode). Touch input handling, gesture
stabilization, row hit-testing and semantic input events are unchanged —
only the two library renderers stop drawing the marker. Host re-validation
after this change: library dumps no longer contain marker pixels and all
band/margin checks still pass; UI Core, input/touch, FreeType/text and
framebuffer/primitives regressions remain green.

## Implementation notes

- The row highlight originally filled `x 32..568`; host validation caught
  a one-column intrusion into the right margin, so the fill now ends at
  `x 567` (the content column) and the validator enforces clean margins.
- Fixture-session expectations are derived from the real deterministic
  scroll: after PREV/NEXT the viewport is `sel=14 top=13`, so the row-3
  tap selects index 16 ("charms"), not 17.
- The library-test binary links `library.c`, and `crossnook-ui-test` now
  links it too (render_selected references `cn_book_format_name`); the
  ui-test behavior and dumps are unchanged because no library is attached.
- The E2E harness opens the input FIFOs with O_RDWR (`<>`) so device-open
  ordering can never deadlock, reaps the qemu child with `wait`, closes
  all descriptors, cleans up on EXIT via a trap, and runs a per-session
  watchdog that kills a stalled child instead of hanging.

## Known limitations / future work

- Books are listed by filename; EPUB/FB2 metadata parsing is future work.
- No paginated two-page layout, read progress, cover extraction or
  settings. Text rendering of book contents is not implemented yet
  (`[Reader not implemented]`).
- E-Ink full-frame refresh latency from the UI Core milestone applies.

## Non-goals (unchanged)

No EPUB/FB2 parsing, no CREngine, no Wi-Fi/suspend/settings/metadata
cache/persistent storage, and no changes to boot/image/rootfs/kernel
mapping or input/display behavior.