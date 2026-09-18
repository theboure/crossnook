# crossnook-test — single-binary framebuffer + input diagnostic

**Hardware status: PASS (verified on real Nook Simple Touch, BNRV300).**
See `docs/milestone-inputapp.md` → "On-device results" for the full
validation checklist. This binary is the known-good on-device baseline
before reader development.

`crossnook-test` is a single static ARM EABI soft-float executable that
validates framebuffer output and evdev input on the Nook Simple Touch
(BNRV300, Linux 2.6.29) without modifying the boot image, kernel, or
rootfs. It can be deployed over ADB and removed with a single `rm`.

---

## Build

From the repository root (toolchain must be available inside Docker):

```bash
bash testapp/build-test.sh
```

This produces `testapp/crossnook-test` — a fully static, stripped,
non-PIE ELF32 ARM binary.

### ELF metadata (reference)

| field          | value                          |
|----------------|--------------------------------|
| type           | `EXEC` (not ET_DYN / PIE)     |
| arch           | ARM, EABI5, soft-float ABI    |
| cpu            | v5T                            |
| static linking | yes (no INTERP / no Dynamic)  |
| flags          | `0x5000200`                    |

The binary contains no shared library dependencies and no PLT/GOT
relocations.  It runs on the unmodified Nook 2.6.29 kernel with only
the standard `/dev` nodes present.

---

## Deploy to device

### Prerequisites

ADB is available and the device is connected (`adb devices` shows it).
The device `/dev` nodes are as expected:

| node                  | role                |
|-----------------------|---------------------|
| `/dev/graphics/fb0`   | framebuffer (960000-byte mmap region) |
| `/dev/input/event0`   | keys: NEXT, BACK    |
| `/dev/input/event1`   | keys: HOME, POWER   |
| `/dev/input/event2`   | touchscreen (ABS_X, ABS_Y, BTN_TOUCH) |

### Push and run

```bash
adb root
adb push testapp/crossnook-test /data/crossnook-test
adb shell chmod 755 /data/crossnook-test
adb shell /data/crossnook-test
```

To remove:

```bash
adb shell rm /data/crossnook-test
```

---

## Behavior

On launch the binary opens the three input device nodes and the
framebuffer device, draws an initial screen, and enters a `poll()`-based
event loop.  No threads, no timers, no `select()`, no `pthread` —
the entire program is a single-threaded synchronous loop.

### Event handling

Raw `input_event` records are read from the device nodes one at a time
(16 bytes per record, little-endian ARM ABI layout: `sec[4]`,
`usec[4]`, `type[2]`, `code[2]`, `value[4]`).  Each event is mapped
through `inputmap_map()` to a semantic action, then dispatched to
`handle_sev()`.

If more than one device has data in the same `poll()` wake, events are
processed in device-file-descriptor order (event0, event1, event2).
Only the first touch-contact event after a `SYN_REPORT` with
`BTN_TOUCH=1` is used; subsequent ABS coordinates before the next
`SYN_REPORT` are ignored.  This prevents phantom interpolation between
presses.

### Mapping

| input source                | semantic action   |
|-----------------------------|-------------------|
| event0: KEY_NEXT (407)      | `SEV_PAGE_NEXT`   |
| event0: KEY_PREVIOUS (412)  | `SEV_PAGE_PREV`   |
| event0: KEY_BACK (158)      | `SEV_BACK`        |
| event1: KEY_HOME (102)      | `SEV_HOME`        |
| event1: KEY_POWER (116)     | `SEV_POWER`       |
| event2: ABS_X, ABS_Y + BTN_TOUCH=1 + SYN_REPORT | `SEV_TOUCH` |

Unmapped codes (`KEY_MENU`, `KEY_SEARCH`, etc.) are silently dropped.

### Screens and pages

Two screens exist: `MAIN` and `HOME`.  `SEV_HOME` toggles between
them.  Both screens show a page counter (`PAGE N` where N advances
mod 255) and a navigation hint footer.

Diagnostic overlays (toggled independently per screen):

| action    | visual on MAIN              | visual on HOME                 |
|-----------|-----------------------------|--------------------------------|
| MENU      | black band top-right, thin black bar at bottom | *(same)* |
| BACK      | *(no effect on MAIN)*       | 10×10 checkerboard bottom-left |

Touch (any screen): black filled square (13×13 px) centered on the
touch point, with a white 3×3 px dot in the center so the marker is
always visible against either background color.

### POWER long-press exit

The only way to exit the program on-device is a **POWER long-press**:

1. KEY_POWER press (value=1) starts a monotonic timer.
2. KEY_POWER release (value=0) computes elapsed ms since the press.
3. If elapsed ≥ **2000 ms**, the program sets an internal exit flag and
   draws one final frame showing the elapsed duration and the word
   `LONG` instead of `SHORT`.
4. On the next iteration the event loop breaks, file descriptors are
   closed, and the process exits with status 0.

A short press (elapsed < 2000 ms) is logged to stderr but does not
exit.

This threshold is documented so that automated scripts and human testers
know exactly what to do: hold POWER for at least 2 seconds, then
release, and the program will exit.

---

## Non-goals

- **No kernel module, no rootfs modification, no boot image change.**
  The binary is a user-space diagnostic only.
- **No POSIX-conformance guarantee on 2.6.29.**  musl upstream states
  POSIX conformance requires `>=2.6.39`; this binary uses only `open`,
  `read`, `write`, `lseek`, `poll`, and basic libc facilities that
  happen to work on 2.6.29.  If the Nook smoke test passes, that is
  sufficient.
- **No graphics acceleration, no font files, no external data files.**
  Everything is compiled into the single ELF binary (embedded 5×7 bitmap
  font, 95-glyph ASCII table).

---

## Host-side validation (no device required)

Two validation scripts run the real ARM binary under `qemu-arm` inside
the pinned Docker toolchain image.

### `testapp/build-test.sh`

Runs the build, verifies ELF metadata, and executes the binary's
built-in selfcheck mode (writes a 960000-byte framebuffer file
`testapp/fb-selfcheck.bin` with a known page number, diagnostic bands,
and touch marker; prints a checksum for consistency).

### `testapp/host-live-test.sh`

End-to-end integration test: creates FIFOs standing in for the three
input device nodes and a regular file for `/dev/graphics/fb0`, starts
the real ARM binary under `qemu-arm`, then feeds a scripted event
sequence through the FIFOs in one-process-per-device-writer mode:

1. `KEY_NEXT` × 1 → page 2
2. `KEY_HOME` → HOME screen
3. `KEY_NEXT` × 1 → page 3
4. Touch at `(100, 50)` with `SYN_REPORT`
5. `KEY_BACK` → BACK diagnostic toggled on
6. `KEY_POWER` held for 3 seconds then released → LONG → program exits

After the program exits, the script:
- copies the written framebuffer to `testapp/fb-live.bin`
- verifies the `POWER long-press, exiting` message appears in stderr
- verifies fb0 is exactly 960000 bytes
- verifies the marker square is centered at (100, 50)
- verifies the MENU band is absent (diag_menu off)
- verifies the checkerboard is present (diag_back on)
- verifies the PAGE text band differs from the page-7 selfcheck

All checks are host-side; no Nook hardware is needed.

---

## Screenshots

`testapp/fb-selfcheck.png` — rendered from `fb-selfcheck.bin` (RGB565 →
24-bit PNG, host-side), showing the initial page with all diagnostic
bands and the touch marker centered at (300, 400).  Useful as a visual
reference for what "page 1, MENU+BACK on" looks like.
