# Milestone: single native ARM test app (fb + input)

**Status: HARDWARE VALIDATION PASS** — verified on real Nook Simple Touch (BNRV300).

Smallest next milestone after input mapping. One native ARM binary, deployed
over ADB to the **current diagnostic environment**, that combines the two
already-verified paths:

- framebuffer write (`/dev/graphics/fb0`, 600x800 RGB565, raw 960000-byte
  write triggers E-Ink refresh — confirmed);
- evdev input (`/dev/input/event0/1/2` — mapping confirmed in
  `diag/README.md`).

It is an on-device **smoke test of fb+input together**, not a UI framework.

## Goal

`crossnook-test`:

1. displays a simple CrossNook test screen on the framebuffer;
2. page buttons (left upper/lower, right upper/lower) visibly change
   something on screen (e.g. a page-number / highlight bar moves);
3. Home button changes screen/state (visible header/state text swap);
4. a touch draws or moves a visible marker at the touched coordinate.

No boot image / rootfs / boot blobs are modified. No rotation or calibration
transform — coordinates are used 1:1.

## Confirmed facts (inputs this milestone depends on)

| fact | measured value |
| --- | --- |
| fb device | `/dev/graphics/fb0` (omap3epfb, 600x800, bpp 16, stride 1200) |
| fb write | 960000-byte raw RGB565 frame -> visible refresh (no mmap required) |
| event0 (TWL4030 Keypad) | 412 `KEY_PREVIOUS` (left upper), 139 `KEY_MENU` (left lower), 407 `KEY_NEXT` (right upper), 158 `KEY_BACK` (right lower) |
| event1 (gpio-keys) | 102 `KEY_HOME` (Home), 116 `KEY_POWER` (Power) |
| event2 (zForce Touchscreen) | `ABS_X` horiz, `ABS_Y` vert, `BTN_TOUCH` 1/0, `SYN_REPORT` frame end |
| device ABI | ARM EABI5, soft-float (`e_flags=0x5000002` on busybox/adbd/init) |
| adbd | runs as root on diagnostic image -> `/dev/input/*`, `/dev/graphics/*` readable |

## App behavior spec

- Single C file (plus a tiny shared mapping header, see Design) — pure
  POSIX: `open/read/write/poll`, no external libs, **statically linked**.
- Main loop: `poll()` on the three `event` fds + optional small sleep;
  decode `struct input_event` (little-endian, 16 bytes:
  two `__s32` timespec + `__u16 type` + `__u16 code` + `__s32 value`).
- State machine with 2-3 states/screens to fully exercise page + Home:
  - **screen A (test grid + page bar)**: page button increments/decrements a
    number shown near the bottom; Home switches to screen B.
  - **screen B (Home panel)**: shows state text + all button labels;
    page button returns to A; Home toggles a "state=OK/BUSY" badge.
  - On any screen, a `BTN_TOUCH=1 + ABS_X/ABS_Y` frame draws/moves a filled
    marker square at `(ABS_X, ABS_Y)`, clamped to 600x800, and refreshes.
- E-Ink refresh: write the full 960000-byte front buffer to fb0 after each
  user-visible change (matches the confirmed write path; partial refresh is
  explicitly out of scope for this milestone).
- Exit on a long press of Power or on SIGTERM (so we stop cleanly and don't
  fight adbd/next runs).

## Design notes

- Input **mapping layer** is kept in one `inputmap.h` (eventN -> {type,code}
  -> logical action), mirroring the briefing's "input mapping layer" rule
  without the full abstraction; the app body handles only logical actions
  (`PAGE_NEXT`, `PAGE_PREV`, `HOME`, `TOUCH_XY`).
- UI renders into a `uint16_t fb[600*800]` ram buffer, then one `write()`;
  matches the briefing's "render to buffer, then copy+refresh" direction.
- No mmap/ioctl this milestone (not confirmed on device; plain write is).
  `FB*` ioctls (clean refresh, inversion) are a separate later milestone.

## Build (how we will produce the binary)

Device binaries are ARM EABI5 soft-float. Windows has no ARM toolchain, so
compile in Docker (Docker Desktop present, v28.4.0) with a pinned
`arm-linux-gnueabi`-class container, static linking:

```bash
docker run --rm -v "$PWD:/io" IMAGE gcc -static -O2 -Wall testapp/testapp.c \
    -o testapp/crossnook-test
```

`IMAGE` TBD and pinned during implementation (e.g. `dockcross/linux-armv5`
or an `armel` Debian cross image); the chosen image + gcc version are then
recorded in `work/manifest.json`-style provenance. Static linking avoids any
dependency on the device's libc version.

## Deploy + verify (Definition of Done)

```bash
adb push testapp/crossnook-test /data/crossnook-test
adb shell chmod 755 /data/crossnook-test
adb shell /data/crossnook-test &
adb shell kill %1        # stop (or long-press Power)
```

Expected on the device:

1. CrossNook test screen visible on the E-Ink.
2. Left/right page buttons move the page bar / number in the correct
   direction (button-to-action direction confirmed by eye).
3. Home swaps screen/state visibly.
4. Each tap moves the marker to the tapped spot (no X/Y swap, matches
   top-left/center/bottom-right taps).
5. No visible artifacts beyond the intended draw areas.

## Non-goals (kept out on purpose)

- No boot image / rootfs / init changes; no change to the diagnostic card.
- No mmap, no FB ioctls, no partial refresh, no rotation/calibration.
- No on-screen text rendering yet (no font engine this milestone) — the
  "page number" is drawn as glyph-like pixel bars, not text.
- No suspend/power handling, no Wi-Fi.

## On-device results (all PASS)

All items below verified on real Nook Simple Touch (BNRV300, Linux 2.6.29):

| check | result |
|-------|--------|
| native ARM binary executes | PASS — statically linked EABI5, runs directly under ADB |
| framebuffer rendering | PASS — full 960000-byte RGB565 write triggers visible E-Ink refresh |
| NEXT button (right upper) | PASS — page counter increments visibly |
| PREVIOUS button (left upper) | PASS — page counter decrements visibly |
| HOME button | PASS — toggles MAIN ↔ HOME screen |
| BACK button (right lower, event0 158) | PASS — BACK diagnostic overlay toggles on HOME screen |
| MENU button (left lower, event0 139) | PASS — MENU diagnostic overlay toggles on both screens |
| zForce touchscreen | PASS — black marker square appears at tapped coordinate, no X/Y swap |
| coordinate accuracy | PASS — top-left / center / bottom-right taps land at expected positions |
| long POWER exit | PASS — hold POWER ≥ 2 s, release → program exits cleanly, fd closed |
| no device corruption | PASS — no changes to eMMC, boot image, or rootfs |

## Known-good baseline

This is the last point in the repository validated on real hardware before
reader development begins. The following are stable and should not be
modified without re-validation on device:

- `testapp/crossnook-test.c` — app behavior and event loop
- `testapp/inputmap.h` — input mapping layer
- `testapp/build-test.sh` — build pipeline
- `testapp/fb-selfcheck.bin` — selfcheck reference output (page 7, MENUDIAG+BACKDIAG)
- `toolchain/Dockerfile` — pinned musl.cc cross-toolchain image
- `docs/toolchain-smoke.md` — toolchain provenance record

## Hardware assumptions not yet confirmed

All I/O here uses the already-confirmed paths. Only assumptions: repeated
full-buffer writes remain stable, and simultaneous readers on evdev are
fine (confirmed practice, but long-run behavior untested).