# CrossNook toolchain smoke test

**Scope: this is the staged gate before `crossnook-test`.** It proves a
binary produced by a *pinned* cross toolchain runs on the Nook Simple Touch
(Linux 2.6.29, ARMv7). No boot image / rootfs changes.

## Toolchain identity (pinned)

| component | value |
| --- | --- |
| cross compiler | `arm-linux-musleabi-cross` from musl.cc |
| gcc | 11.2.1 20211120 |
| binutils | 2.37 |
| libc | musl `1.2.2-git-50-gb76f37fd` (determined at runtime from `__libc_version`) |
| source archive | `https://musl.cc/arm-linux-musleabi-cross.tgz` |
| sha256 (pinned download) | `d70c607101fee5330083463feacf5992892a85b826d1626094500e0c37ec7d25` |
| archive location | `work/toolchain/arm-linux-musleabi-cross.tgz` |

> musl.cc URLs are mutable; the pinned artifact is the local copy recorded
> above. The Dockerfile re-verifies its sha256 at image build time.

### Why musl and why this ABI

- musl targets Linux 2.6.x syscall ABI; upstream: *"musl is built on the
  Linux syscall layer. Linux kernel >=2.6.39 is necessary for POSIX
  conformant behaviour, older kernels will work with varying degrees of
  non-conformance"* (wiki.musl-libc.org). For our exploratory
  single-threaded program (open/poll/read/write/lseek/exit only, no
  threads, no `*at` dependence) 2.6.29 is an explicitly supported regime —
  which is exactly what the on-device smoke test verifies.
- The device's own binaries are ARM EABI5, **soft-float** (busybox/adbd
  `e_flags=0x5000002` -> Version5 EABI, soft-float). `arm-linux-musleabi`
  matches that ABI exactly. Hard-float (`musleabihf`) builds would not be
  ABI-compatible with the existing rootfs and are not used.
- Fully **static** link: no dependence on the device's libc at all.

## Build (host-side)

Requires Docker (Desktop on Windows). Everything runs in the pinned image;
no ARM toolchain is installed on the host.

```bash
# from the repo root: build image, compile smoke, print validation
bash toolchain/build-smoke.sh
```

What the script does:

1. `docker build` the pinned toolchain image (verifies the sha256, unpacks
   the compiler into `/opt/tc/...`, installs `file`, `binutils` and
   `qemu-user` for host-side validation).
2. Compiles `toolchain/smoke/smoke.c`:
   ```bash
   arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -o smoke smoke.c
   ```
3. Prints gcc/musl identity, `file`, ELF metadata and runs the binary under
   `qemu-arm`.

### Host-side validation (this build)

```
arm-linux-musleabi-gcc (GCC) 11.2.1 20211120
musl libc version: 1.2.2-git-50-gb76f37fd
file smoke:
  ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV),
  statically linked, with debug_info, not stripped
readelf -h:
  Type:   EXEC (Executable file)
  Machine: ARM
  Flags:  0x5000200, Version5 EABI, soft-float ABI
readelf -l:
  no INTERP / no Dynamic section  -> fully static, non-PIE
readelf -A:
  Tag_CPU_arch: v5T
run via qemu-arm:   CrossNook toolchain OK
```

Key facts claimed by the archive, all confirmed by readelf:
- ELF32 little-endian ARM; EABI5; **soft-float ABI** flag;
- static EXEC (Type EXEC, no `INTERP`, no `DYNAMIC`) — PIE is deliberately
  disabled (`-no-pie`) because 2.6.29 predates reliable static-PIE loading;
- smoke binary sha256:
  `24048c3dc1d877bb8db9f1899589d63c6462b7df72796380119087ac077d5868`
  (also printed by `build-smoke.sh`).

`__MUSL__` is not emitted as a preprocessor macro by this toolchain's
sysroot; the libc version was instead read from the `__libc_version[]`
symbol at runtime (`toolchain/smoke/libcver.c`), which is the pinned
identity recorded above.

## Push and run on the Nook (Definition of Done)

The binary is `toolchain/smoke/smoke` (static, ~18 KB).

```bash
adb push toolchain/smoke/smoke /data/smoke
adb shell chmod 755 /data/smoke
adb shell /data/smoke
```

Expected output on the device:

```
CrossNook toolchain OK
```

and `adb shell` returning to the prompt (exit code 0).

If `/data` is read-only on the diagnostic image, use `/tmp` instead and add
`-t` if needed. If the binary segfaults or prints nothing, capture
`adb shell dmesg | tail -20` and stop — that would reset the toolchain
choice.

## Gate

`crossnook-test` may be **built and host-validated** in parallel, but must
**not be deployed to the Nook** until `/data/smoke` prints
`CrossNook toolchain OK` on the real device. This milestone is deliberately
a single small binary with zero device I/O so a toolchain/kernel-ABI failure
cannot be confused with an fb/input problem.