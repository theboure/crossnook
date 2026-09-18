# crossnook-ui-test — UI Core composition diagnostic

**Status: HARDWARE VALIDATION PASS** (real Nook Simple Touch). Screens,
navigation, touch coordinates, semantic input events and the long-POWER
exit confirmed on-device.

`crossnook-ui-test` composes the extracted UI Core modules — framebuffer,
input, primitives graphics, FreeType text and a minimal UI state layer —
into one native ARM binary. It is the first crossnook piece with a
reusable `src/` module tree instead of a single monolithic `.c`.

## Deliverables

| file | purpose |
| --- | --- |
| `src/graphics/canvas.{c,h}` | generic RGB565 memory canvas: clear, fill/outline rect, point, h/v line, clipping, per-channel alpha blend |
| `src/graphics/text.{c,h}` | FreeType textual extraction (UTF-8, sizes, baselines, wrap, spacing, blending, Cyrillic) |
| `src/platform/nook/display.{c,h}` | sole owner of `/dev/graphics/fb0` (open + full-frame 960000-byte flush) |
| `src/platform/nook/input.{c,h}` | poll + semantic mapping (page/menu/back/home/power/touch with frame aggregation) |
| `src/ui/ui.{c,h}` | UI state layer: HOME / READER_TEST, page counter, touch marker, long-POWER exit |
| `src/app/ui-test.c` | executable: device loop + host modes (`--smoke/--home/--reader/--primitives`) |
| `testapp/build-ui.sh` | cross-compile + metadata + structural + host-mode + validator + E2E |
| `testapp/host-live-ui-test.sh` | FIFO/qemu scripted-input interaction test |
| `testapp/validate-ui.py` | dump checks + PNG previews |

## Module boundaries (the point of this milestone)

- **display** is the ONLY code that opens the framebuffer node. `build-ui.sh`
  structurally asserts the string `"/dev/graphics/fb0"` appears in no `.c`
  file except `src/platform/nook/display.c`. Nothing else may peek at raw
  repaint/E-Ink controls, mmap the fb, or write partial frames.
- **input** is the only place that knows about `event0/1/2` and raw
  key/abs codes; consumers see only `cn_input_ev` semantic events
  (PAGE_*, MENU, BACK, HOME, POWER_DOWN/UP, TOUCH_DOWN/MOVE/UP with coords).
  Touch is aggregated: zForce's ABS_X/ABS_Y/BTN_TOUCH/SYN_REPORT frames
  become DOWN (provisional — the first and release frames can carry a
  transient coordinate near the opposite edge and are never used as a tap
  position), MOVE (coords changed while in contact), UP (release frame,
  carrying the RESOLVED last in-contact coordinate; a one-frame tap falls
  back to the down coordinate).
- **text** keeps every FreeType type (FT_Face etc.) inside `text.c`; the UI
  state layer never sees FreeType. Behavior is the hardware-validated
  crossnook-text renderer, extracted without redesign (UTF-8 validation,
  grayscale/MONO coverage alpha-blended per RGB565 channel, ascender
  baselines, hinted advance used for both measure and render, word wrap
  only on real width overflow, line advance = 11/10 of line height,
  32 px margins, ink/bbox statistics preserved for regression).
- **canvas** is pure memory: no device nodes, no platform headers.

A structural check in `testapp/build-ui.sh` (`grep -rl '"/dev/graphics/fb0"'
src --include='*.c'`) fails the build if the framebuffer literal ever
escapes `display.c` (comments are allowed; runtime opens are not).

## The UI (HOME / READER TEST)

```
HOME:        CrossNook            (48 px)
             UI Core              (24 px)
             [ Open reader test ] (24 px, outlined button)

             NEXT or touch in the button -> READER TEST

READER TEST: Reader test          (36 px)
             Page N               (24 px)  N = 1,2,3...
             <Cyrillic pangram>   (24 px, wrapped)
             NEXT/PREV page  BACK/HOME home  PWR>2s exit   (14 px footer)
             touch draws a box+dot marker
```

- POWER press..release: `release_time - press_time >= 2000 ms` requests
  exit (monotonic clock, as validated in crossnook-test).
- Touch actions (marker, HOME button) are committed on TOUCH_UP using the
  resolved coordinate, never on the provisional TOUCH_DOWN.
- State transitions logged to stdout (`UI state=...`, `UI touch=...`
  — touch logged only on TOUCH_UP, `UI long-power exit`) and recognized
  inputs traced to stderr (`ui-test: INPUT MENU/BACK/HOME/POWER_DOWN/
  POWER_UP`) so the scripted E2E and hardware runs can assert them.

## Build + host validation

```bash
bash testapp/build-ui.sh          # build + structural + smoke + dumps + E2E
python testapp/validate-ui.py testapp/fb-ui-home.bin HOME
```

`build-ui.sh` runs, inside the pinned docker toolchain image:

1. Cross-compile the 6 module files with `-Isrc -lfreetype -static
   -no-pie -fno-pie -O2 -Wall -Wextra`; verify `file`/`readelf` (static
   EXEC, ARM EABI5, soft-float, no INTERP/Dynamic).
2. Structural check: the fb0 literal lives only in `display.c`.
3. Host modes under qemu-arm:
   - `--smoke <font>` — UI logic: HOME render ink, NEXT->READER page
     increments and PREV floor of 1, BACK/HOME return, HOME button hit
     test (in-button tap DOWN+UP navigates, out-of-button does not),
     touch marker commits on UP only (absent on DOWN, pixels at (300,400)
     after UP), short POWER does not request exit.
   - `--home <font> <out.bin>` / `--reader <font> <out.bin> [page]` —
     deterministic screen dumps (960000 bytes) + `RENDER` stats line.
   - `--primitives <out.bin>` — in-app canvas assertions (clip, fill,
     outline, h/v line, blend midpoints, OOB safety) + dump.
4. `validate-ui.py` pixel checks per kind (see below).
5. `host-live-ui-test.sh` — full E2E under qemu with FIFO input devices.

`host-live-ui-test.sh` recreates the crossnook-test harness for this app:
FIFOs at `/dev/input/event0..2`, a touch file at `/dev/graphics/fb0`, feeds
a scripted sequence (NEXT x2, PREV, HOME, a one-frame normal tap (51,33),
the exact bottom-edge zForce transient sequence (300,9 down → 301,781
move → 300,9 release, resolved to (301,781) — never (300,9)), a multi-move
tap (resolved (220,320)), MENU press, BACK, POWER held 3 s) and asserts
the app's stdout log: `UI initial state=HOME page=1 inputs=3`;
`state=READER page=2/3/2`; `state=HOME`; `UI touch=51,33`;
`UI touch=301,781` (absent: `UI touch=300,9`);
`UI touch=220,320`; `state=READER page=3`; `UI long-power exit` — plus
stderr traces: `INPUT MENU`, `INPUT BACK`, `INPUT HOME`,
`INPUT POWER_DOWN`, `INPUT POWER_UP`; plus a 960000-byte final
framebuffer frame.

Current host results (deterministic):

```
SMOKE state=HOME page=1 bad=0 -> OK
RENDER HOME  size=960000 checksum=3fba3097 ink=5540 bbox=(34,25)-(286,240)
RENDER READER size=960000 checksum=3aede374 ink=9344 bbox=(33,23)-(516,785)
PRIMITIVES checksum=4dfd462d failures=0 -> OK
host-live: all asserts OK, fb frame 960000 bytes
```

`validate-ui.py` asserts: file size 960000; content present; pixel bbox
inside screen. Per kind: HOME has title ink, button-outline ink in its y
band and nothing below the button; READER has title, Cyrillic body and
gray footer; PRIMITIVES pins specific pixels (gray fill + box + clip
boundary, outline border vs interior, hline end, vline end). PNG previews
are written alongside the dumps.

## Deploy to the Nook

```bash
adb push testapp/crossnook-ui-test /tmp/crossnook-ui-test
adb push testapp/test-font.ttf /tmp/test-font.ttf
adb shell chmod 755 /tmp/crossnook-ui-test
adb shell /tmp/crossnook-ui-test /tmp/test-font.ttf
```

Expected: a HOME screen (CrossNook / UI Core / [ Open reader test ]).
NEXT (or the button) opens the reader test; NEXT/PREV page; BACK/HOME
return home; touch shows a marker at the tap point; hold POWER > 2 s to
exit. Menus and short POWER are recognized and logged as `INPUT <NAME>` on
stderr but deliberately do nothing. Remove afterwards:
`adb shell rm /tmp/crossnook-ui-test /tmp/test-font.ttf`.

### Known limitation: E-Ink refresh latency

Semantic touch/UI events appear immediately in ADB, while the visible
E-Ink screen update occurs noticeably later. This is a display-refresh
latency limitation, not an input failure: input and UI state react at
input speed; only the panel repaint trails. The milestone intentionally
uses full-frame framebuffer writes and no partial-refresh policy. An
E-Ink partial-refresh / dirty-region repaint policy is a future
optimization item (see `docs/milestone-ui-core.md`).

## Regressions

The two earlier diagnostics still build and validate unchanged
(`bash testapp/build-test.sh`, `bash testapp/build-text.sh`); the text
render checksum remains `e45d6ba9`.

## Non-goals

No EPUB, CREngine, CSS, hyphenation, font-settings UI, library scanning,
partial E-Ink refresh, power management, Wi-Fi, or rootfs/boot image
changes.