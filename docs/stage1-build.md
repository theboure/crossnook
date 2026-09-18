# CrossNook stage-1 microSD build

Reproducible build of a bootable microSD card for a Nook Simple Touch
(BNRV300). Stage-1 goal: boot the device to Linux userspace with a BusyBox
shell — **without touching internal eMMC** — while keeping normal boot
working once the card is removed.

Everything needed is rebuilt from a single pinned source (the known-good
`NookManager1.2.2.img`) and the produced card image is byte-verifiable.

## What gets built

```
work/crossnook-stage1.img  64 MiB FAT32 "superfloppy" (no MBR/MBR boot code)
   MLO                      boot ROM loader      (reused verbatim, pinned)
   U-BOOT.BIN               u-boot              (reused verbatim, pinned)
   UIMAGE                   kernel              (reused verbatim, pinned)
   URAMDISK                 built: minimal rootfs -> newc cpio -> gzip -> uImage
   BOOT.SCR                 built u-boot script (uImage type 6; content
                            byte-identical to the reference BOOT.SCR payload)
```

The rootfs is assembled from **proven binaries taken verbatim** from the
reference image's own ramdisk (Android `init`, `busybox`, `adbd`,
`toolbox`/`linker`+`libc` etc.), over which we place our own control files:
`init.rc`, `default.prop`, `sbin/bootlog.sh`. No binary is compiled in
stage 1; custom kernel/rootfs cross-compilation is a later stage.

Why Android `init` as `/init`: the shipped kernel has no devtmpfs, and this
`init` mounts `/dev`, `/proc`, `/sys`, `/dev/pts` itself and cold-boots
device nodes — the same path NookManager/stock recovery uses. Our rootfs
does **not** need `mdev -s`.

## Build

Requires: Docker (Desktop on Windows). No u-boot-tools needed — the uImage
header, cpio and FAT32 are produced by the pure-Python toolchain.

```bash
./build/build.sh            # first build downloads + verifies the pinned image
CACHED=1 ./build/build.sh   # offline; requires cached work/NookManager1.2.2.img
```

Without Docker, run the pipeline directly:

```bash
python build/build_stage1.py
```

Outputs:

| artifact | note |
| --- | --- |
| `work/crossnook-stage1.img` | flash this to the card |
| `work/manifest.json` | sha256 of every input and of the card image |
| `work/stage1-rootfs/` | assembled minimal rootfs (for inspection) |
| `work/ref-rootfs/`, `work/ref-blobs/` | unpacked reference material |

The build re-verifies its own output by re-reading the finished image and
re-checking every blob hash; the embedded cpio archive is parsed back and
`boot.scr`/geometry are validated before the build reports success.

## Flash

- Write the card (>= 64 MiB, everything on it is discarded):

  - Linux: `sudo dd if=work/crossnook-stage1.img of=/dev/sdX bs=4M status=progress`
  - Windows: Win32DiskImager or Raspberry Pi Imager ("custom image",
    but RPi Imager may not accept a bare .img; Win32DiskImager is simplest).

- Verify: `sudo sha256sum /dev/sdX` for the first 67108864 bytes should equal
  `card_image_sha256` in `work/manifest.json` (of the *card image*, before dd;
  see note below).

> sdX must be the SD card, not a system disk. Double-check with
> `lsblk`/Disk Management first. After flashing, `lsblk` should show a single
> FAT partition (`CROSSNOOK`) filling the card.

## What happens at boot

```
OMAP boot ROM -> MLO -> u-boot
  -> bootcmd: autodetectmmc; loadbootscript (fatload boot.scr from SD)
  -> autoscr: setbootargs / mmcinit / fatload UIMAGE@0x81c00000 /
              URAMDISK@0x81f00000 / bootm
  -> kernel cmdline:
      console=ttyS0,115200n8 initrd rw init=/init vram=16M
      video=omap3epfb:mode=800x600x16x14x270x0,pmic=...,vcom=...
      androidboot.console=ttyS0
  -> Android init: mounts tmpfs /dev, proc, sys, devpts; coldboots nodes;
     onproperty -> starts adbd; bootlog.sh copies dmesg+init log to the SD.
```

The SD = `mmcblk1` (the card), `mmcblk0` = internal eMMC. Our scripts never
write to `mmcblk0`. Remove the card and the device boots its normal Android
firmware.

Interfaces to use for verification:

| channel | use |
| --- | --- |
| UART ttyS0 @ 115200 8N1 | kernel + init log output |
| USB (adb) | `adb devices` / `adb shell` shell over USBDWC |
| SD card | `boot.log` (created by `bootlog.sh`) |

## Verify (Definition of Done)

1. Activity: cold-boot the NST with the card inserted (hold power ~30 s).
2. UART shows u-boot banner then kernel messages on `ttyS0`.
3. `adb devices` lists the device; `adb shell` gives a running shell
   (`/ #`).
4. `mount` shows root on initramfs, `/proc`, `/sys`, `/dev` present.
5. Eject/remove card, reboot: device returns to stock Android boot
   (no boot loop, eMMC unchanged).

Remaining device-verify unknowns (see `docs/boot-research.md`): exact
`epd_pmic`/`vcom` values for the built-in `setbootargs`, whether the stock
product needs `product.device=...`/`ro.product.udid=` props for adbd to
start cleanly, and the precise USB gadget mode adbd brings up.

## Design constraints

- Never write to internal eMMC; SD-only writes.
- Reuse proven binaries verbatim; pin and verify every input hash.
- Deterministic output (fixed timestamps) so the same inputs yield the same
  bytes; the only variable is the gzip header timestamp, which is frozen to 0.
- Small iterations: each stage is verified on-device before moving on.