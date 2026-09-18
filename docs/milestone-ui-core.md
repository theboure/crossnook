# Milestone: UI Core (reusable module tree + minimal UI state)

**Status: HARDWARE VALIDATION PASS** (real Nook Simple Touch). Screens,
navigation, touch coordinates, semantic input events and the long-POWER
exit all confirmed on-device. Tagged `milestone/ui-core`.

## Scope (what this milestone is)

Extract the hardware-validated framebuffer (crossnook-test), input mapping
(crossnook-test + inputmap.h), primitive graphics, and FreeType text
(crossnook-text) into a reusable module tree under `src/`, and compose
them in a minimal UI state layer proving the pattern:

```
src/platform/nook/display.{c,h}   sole owner of /dev/graphics/fb0
src/platform/nook/input.{c,h}     poll + semantic events, touch aggregation
src/graphics/canvas.{c,h}         RGB565 memory canvas (no hardware)
src/graphics/text.{c,h}           FreeType extraction (no FT types outside)
src/ui/ui.{c,h}                   HOME / READER_TEST state layer
src/app/ui-test.c                 executable + host modes
```

New executable `crossnook-ui-test`; `crossnook-test` and `crossnook-text`
remain as regression references (byte-identical renders preserved).

## Host validation results (this build)

- ELF: static EXEC, ARM EABI5, soft-float (Flags 0x5000200), no
  INTERP/Dynamic.
- Structural: `"/dev/graphics/fb0"` string appears only in
  `src/platform/nook/display.c`.
- `--smoke`: state machine checks pass (NEXT->READER, page floor,
  BACK/HOME, button hit-test, marker pixels, short-POWER no-exit).
- `--home` / `--reader`: deterministic dumps, checksums `3fba3097` /
  `3aede374` (page 1) vs `3aec7948` (page 3) — pages differ.
- `--primitives`: 0 assertion failures (clip, fill, outline, h/v line,
  blend midpoints, OOB safety).
- `validate-ui.py`: HOME / READER / READER3 / PRIMITIVES all green
  (title, button outline, Cyrillic body, footer, geometry pixels).
- `host-live-ui-test.sh` E2E: all asserted state transitions, touch
  coordinates (including the exact bottom-edge zForce transient sequence
  `300,9 -> 301,781 -> 300,9`, which must resolve to `(301,781)` and never
  `(300,9)`), the semantic `INPUT MENU/BACK/HOME/POWER_DOWN/POWER_UP`
  stderr traces, and the long-POWER exit observed; final fb frame
  960000 bytes.
- Regressions: `build-test.sh` and `build-text.sh` still pass; text
  render checksum unchanged (`e45d6ba9`).

## On-device validation results (real Nook Simple Touch)

1. HOME: white screen, "CrossNook", "UI Core", "[ Open reader test ]" —
   PASS.
2. NEXT and the touch button both open READER TEST — PASS.
3. NEXT/PREV change the page; page never drops below 1 — PASS.
4. BACK and HOME return to HOME — PASS.
5. Touch draws the black box + white dot marker at the touch point;
   coordinates are correct — PASS.
6. Bottom-edge zForce transient coordinates are filtered correctly —
   PASS.
7. MENU/BACK/HOME/POWER semantic events are all recognized — PASS.
8. Hold POWER >= 2 s exits (binary terminates) — PASS.

Deploy: see `docs/crossnook-ui-test.md` ("Deploy to the Nook").

## Implementation notes

- **Extraction, not redesign**: `text.c` is the validated crossnook-text
  renderer (identical UTF-8 validation, per-channel alpha blend, ascender
  baselines, hinted advance for measure+render, wrap-on-overflow with
  progress guarantee, 11/10 line advance, 32 px margins, Cyrillic). The
  ink/bbox stats survive as `cn_text_stats` for regression comparison.
- **display owns the fb**: `cn_display_open/flush/close`; flush is the
  proven lseek(0) + partial-write-safe loop of exactly 960000 bytes. No
  mmap/ioctls/sysfs/partial refresh.
- **input hides rawness**: `cn_input_poll()` returns semantic events.
  Key press/release: page/menu/back/home emit on press; POWER reports
  both down and up (the UI times the hold itself). Touch is aggregated
  for zForce's real behavior: the very first down frame and the release
  frame can carry a transient coordinate near the opposite edge (e.g.
  `300,9` down, `301,781` move, `300,9` up for a real touch near
  `(298,780)`). TOUCH_DOWN therefore carries provisional coordinates;
  the running contact is tracked on every later in-contact SYN_REPORT
  (TOUCH_MOVE on change); the up frame ends the gesture and TOUCH_UP
  carries the RESOLVED coordinate — the last in-contact position before
  release, falling back to the down frame for a one-frame tap. Release
  frame coordinates are never used. All coordinates are clamped to the
  600x800 bounds. The UI commits tap actions (marker, HOME button
  navigation) only from TOUCH_UP. Events are buffered in a small
  internal queue because one poll() can yield many records, and semantic
  events are observable: the app traces recognized-but-no-op inputs
  (MENU, short POWER) to stderr as `INPUT <NAME>` so hardware validation
  can tell "recognized" from "not recognized".
- **UI state layer**: two states; NEXT on HOME opens the reader (kept at
  page 1 + increment), PREV floors at 1, BACK/HOME go home, touch
  navigates only via the HOME button and otherwise draws the marker,
  POWER >= 2000 ms (monotonic) requests exit. `cn_ui_render` draws only
  into a `cn_canvas`; the app decides when to flush — enabling the exact
  same render path on host and device.
- **Host E2E fidelity**: the device loop runs unmodified under qemu-arm
  with FIFO input devices and a file framebuffer; the app's stdout logs
  every state/touch transition, which `host-live-ui-test.sh` asserts.

## Known limitations / future work

- **E-Ink refresh latency (display, not input).** Semantic touch/UI events
  appear in ADB immediately, but the visible E-Ink screen update occurs
  noticeably later. Root cause: the milestone intentionally writes
  full-frame framebuffers and implements no partial-refresh policy. This
  is a display-refresh latency limitation, not an input failure — input
  and UI state react at input speed; only the panel repaint trails. A
  future optimization item is an E-Ink partial-refresh policy (mark dirty
  regions, repaint only what changed) and/or repaint tuning, deferred to a
  dedicated milestone.

## Non-goals (unchanged)

No EPUB, CREngine, CSS, hyphenation, font-settings UI, library scanning,
partial E-Ink refresh, power management, Wi-Fi, or changes to the boot
image / ramdisk / rootfs / input mapping / framebuffer architecture.