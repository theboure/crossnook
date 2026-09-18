# Input bring-up (diagnostic environment)

Small, read-only input probe for the Nook Simple Touch running the
**diagnostic** image (`crossnook-diag-nm-ramdisk.img`, original NookManager
ramdisk). Nothing here changes the rootfs, the boot image, or the
framebuffer — it only reads `/dev/input/event*`.

Goal: map physical controls -> event device + event codes.

**Status: mapping measured on the real device — see section 2.**

| device | name (from `/proc/bus/input/devices`) |
|--------|----------------------------------------|
| event0 | TWL4030 Keypad |
| event1 | gpio-keys |
| event2 | zForce Touchscreen |

The logger is pure BusyBox (`sh` + `dd` + `od`); no toolchain on the device
is required.

## 1. Push and run

`adbd` runs as **root** on this image, so `/dev/input/*` is readable.

```bash
# from the repo root on the PC
adb push diag/inputlog.sh /data/inputlog.sh
adb shell chmod 755 /data/inputlog.sh

# one device at a time (watch the codes as you press each control)
adb shell /data/inputlog.sh /dev/input/event1

# or raw hex if you prefer to decode by hand
adb shell /data/inputlog.sh --raw /dev/input/event2

# or all three at once into a merged log on the device
adb shell /data/inputlog.sh all /data/input.log
adb shell cat /data/input.log
```

If `/data` is read-only, use `/tmp/inputlog.sh` instead.

To keep a copy on the PC, redirect the stream:

```bash
adb shell /data/inputlog.sh /dev/input/event1 > event1.log
```

### Reading the output

```
<sec>.<usec> <type>   <code>                 value=<n>
1000.123456 EV_KEY  KEY_HOME   (102)      value=1     # 1 = press
1000.223456 EV_KEY  KEY_HOME   (102)      value=0     # 0 = release
1002.000005 EV_ABS  ABS_X      (0)        value=292   # screen x
1002.000006 EV_ABS  ABS_Y      (1)        value=396   # screen y
1002.000007 EV_KEY  BTN_TOUCH  (330)      value=1     # touch down
1002.000008 EV_SYN  SYN_REPORT (0)        value=0     # frame boundary
```

Unknown codes are printed numerically (`KEY_<n>`, `ABS_<n>`) so nothing is
hidden — extend the case tables in the script if a name is missing.

## 2. Confirmed input mapping (measured on Nook Simple Touch)

Measured with `inputlog.sh` on the real device. These are the **measured
facts** the next milestone builds on — record them exactly as-is.

### Buttons

| device | code | name | physical control |
|--------|------|------|------------------|
| event0 | 412 | `KEY_PREVIOUS` | left upper page button |
| event0 | 139 | `KEY_MENU` | left lower page button |
| event0 | 407 | `KEY_NEXT` | right upper page button |
| event0 | 158 | `KEY_BACK` | right lower page button |
| event1 | 102 | `KEY_HOME` | Home button |
| event1 | 116 | `KEY_POWER` | Power button |

All are `EV_KEY`; a control press emits `value=1`, release `value=0` on the
same code.

### Touchscreen (event2 — zForce)

| type | code | meaning |
|------|------|---------|
| `EV_ABS` | `ABS_X` | horizontal coordinate (framebuffer orientation) |
| `EV_ABS` | `ABS_Y` | vertical coordinate (framebuffer orientation) |
| `EV_KEY` | `BTN_TOUCH` | `1` = touch down, `0` = touch up |
| `EV_SYN` | `SYN_REPORT` | terminates an input frame |

Measured taps:

| location | (x, y) |
|----------|--------|
| top-left | approx (51, 33) |
| center | approx (292, 396) |
| bottom-right | approx (572, 775) |

Framebuffer is 600x800, so touchscreen coordinates already correspond
directly to framebuffer orientation:

```
screen_x = ABS_X
screen_y = ABS_Y
```

No rotation or calibration transform is required. Do **not** introduce one;
coordinate mapping is 1:1 with the framebuffer. (Raw panel min/max is not
measured; ABS_X/ABS_Y already arrive in framebuffer-oriented units.)

### How the mapping was obtained

Using the procedure in section 1: press one control at a time with the
logger running, note the `EV_KEY` code each generates, and record the result
as `<control> = <device> / <type> <code>`.

## 3. Non-goals (kept out on purpose)

- No changes to the rootfs, `init.rc`, boot blobs, or `URAMDISK`.
- No framebuffer work — the earlier confirmed path remains
  `write 960000-byte RGB565 -> /dev/graphics/fb0`.
- No input daemon/writer yet; this step only **identifies** devices and codes
  so a later mapping layer can be specified from measured facts.
- No touch rotation/calibration: framebuffer and touch coordinates are
  already 1:1 (`screen_x = ABS_X`, `screen_y = ABS_Y`).
