# Portable Startup Storage Root Selection

**Status: IMPLEMENTATION COMPLETE; HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

This milestone adds a read-only, fail-closed verifier for an explicitly
selected portable application root. It does not mount, format, partition,
provision, or discover a default root. Deployment supplies the expected mount,
whole-device or partition block major/minor, and an existing application root.

## Measured NookManager environment

Physical read-only reconnaissance identified internal MMC at `179:0`
(`/dev/block/mmcblk0`, type `MMC`) and the boot microSD at `179:16`
(`/dev/block/mmcblk1`, type `SD`). Both report `removable=0`: that sysfs bit is
not a card discriminator on the NST. `/rom` and `/data` are mounted from
internal `mmcblk0p2` and `mmcblk0p8`; `/data` is internal eMMC and must never
be a portable storage default.

The current boot card is a whole-device FAT32 superfloppy, not
`mmcblk1p1`: the PC measured 1,018,691,584 bytes (1,989,632 sectors) and
the Nook measured the same sector count. Its FAT volume begins at offset zero,
is about 64 MiB, and a separately performed read-only whole-device mount
showed about 52 MiB free and the CrossNook boot files. NookManager does not
leave it mounted. The existence of `/sdcard`, `/mnt`, or `/media` is not
evidence of a mounted microSD.

The `179:16` identity is a **deployment input for this verified physical
setup**, not a built-in universal meaning for `mmcblk1`. The verifier does
not consult `removable`, assume a partition suffix, or select `/sdcard`.

## Contract

`src/platform/storage_verify.[ch]` defines:

```c
cn_platform_storage_result cn_platform_storage_verify(
    const cn_platform_storage_candidate *candidate,
    cn_platform_storage_verified *verified,
    int *system_errno);
```

The candidate supplies `root`, `mountpoint`, `expected_major`, and
`expected_minor`. Success returns an owned root/mountpoint copy and the
verified identity. On failure the output is zeroed and no fallback is offered.
Paths are absolute and bounded; trailing slashes alone are removed. Filesystem
root, relative paths, `.`, `..`, repeated internal separators, and a candidate
outside the mount's component boundary are rejected.

The verifier reads a bounded `/proc/self/mountinfo` snapshot, decodes Linux
octal-escaped paths, rejects malformed/truncated/duplicate entries, and chooses
the candidate's *longest component-wise matching mount*. That mount must be
exactly the supplied mountpoint; `/` and nested mounts are not acceptable
fallbacks. The mount must be `rw` in both mount and superblock options, have
filesystem root `/`, and name an ordinary supported block filesystem (vfat or
ext2/3/4). A bind mount of a subdirectory, pseudo source, missing mount, or
read-only mount fails closed.

The mountinfo major/minor must equal the supplied expected block identity.
`stat()` on its source must find a block device with matching `st_rdev`. The
mountpoint and pre-existing root must be real directories with `st_dev`
matching that filesystem; `lstat()` rejects symlinks in all path components.
The source may be a whole-device node such as `/dev/block/mmcblk1`.
`system_errno` is optional and captures syscall failures immediately; other
semantic failures leave it zero.

The verifier never issues `mount`, `mkdir`, `chmod`, `open` for writing, or any
storage probe write. `/proc/self/mountinfo` must be present on the target;
absence fails closed. A `cn_platform_storage_source` callback interface is
exposed **only for synthetic host fixtures** so the host gate never inspects
the workstation's mounts or block devices. Production callers use `verify()`.

No diagnostic `/tmp` bypass is implemented. A `/tmp` candidate can be accepted
in production only if it truly belongs to the explicitly expected verified
block mount; the common tmpfs `/tmp` is never silently accepted.

## Startup ownership and composition

Deployment owns mounting and pre-creating `<verified-mount>/crossnook` after
separate positive card identification. This verifier will neither mount the
card nor create that root. Once it succeeds:

```text
deployment-provided mount, device identity, existing root
  -> cn_platform_storage_verify()
  -> cn_storage_layout_init(verified.root)
  -> cn_storage_layout_prepare()
  -> CONFIG -> cn_settings_store_init()/load()
  -> PROGRESS -> cn_progress_store_open()
  -> future application/controller
```

The diagnostic's `--compose` path performs this sequence only after a
successful live verification; it loads Settings (OK or MISSING) and opens
ProgressStore. It never saves settings or a progress record. Neither published
storage module was changed. Future firmware can supply another mount and block
identity without changing Storage Layout, Settings, or ProgressStore.

## Host gate

```bash
bash testapp/build-storage-verify.sh
```

The gate cross-compiles a static ARMv5TE EABI5 soft-float non-PIE diagnostic
and runs synthetic mountinfo/stat fixtures under QEMU. Cases include whole
device, exact mountpoint and child, lookalike prefixes, rootfs fallback,
missing/readonly/nested mount, wrong expected/source `rdev`, missing and
non-directory root, symlink escape, wrong root filesystem, duplicate/bind and
pseudo mounts, malformed/truncated tables, escaped mountpoints, dot paths,
and no diagnostic fallback.

```text
STORAGE VERIFY SMOKE failures=0 -> OK
STORAGE VERIFY HOST VALIDATION OK
```

## Physical hardware validation

The focused ARM diagnostic was deployed only under `/tmp` on the physical Nook.
No `/data` storage was used and no internal eMMC write was intentionally
performed. Validation used the verified external card identity
`/dev/block/mmcblk1` at `179:16` and the disposable mountpoint
`/tmp/crossnook-card`.

With `/sdcard` existing only as an ordinary directory, the verifier rejected it
as missing mounted media:

```text
STORAGE VERIFY result=mount-missing errno=0 root= mount= device=0:0
```

With the external card mounted read-only as `vfat` at `/tmp/crossnook-card`, the
verifier rejected the correct device because it was not writable:

```text
STORAGE VERIFY result=read-only errno=0 root= mount= device=0:0
```

With the same verified card mounted read/write and a pre-existing application
root at `/tmp/crossnook-card/crossnook`, expected device `179:16` succeeded:

```text
STORAGE VERIFY result=ok errno=0 root=/tmp/crossnook-card/crossnook mount=/tmp/crossnook-card device=179:16
```

With the same valid mounted card/root but expected device `179:0`, the verifier
rejected the internal eMMC identity mismatch:

```text
STORAGE VERIFY result=wrong-device errno=0 root= mount= device=0:0
```

After successful storage verification, Storage Layout, CONFIG, Settings Store,
and ProgressStore composed against the verified microSD root:

```text
STORAGE VERIFY result=ok errno=0 root=/tmp/crossnook-card/crossnook mount=/tmp/crossnook-card device=179:16
STORAGE COMPOSE config=/tmp/crossnook-card/crossnook/config progress=/tmp/crossnook-card/crossnook/state/progress settings=missing -> OK
```

After unmounting the card, the same candidate path failed closed again:

```text
STORAGE VERIFY result=mount-missing errno=0 root= mount= device=0:0
```

This physically confirms that an underlying directory does not become a valid
storage root after media removal. The microSD now contains a provisioned
CrossNook application root at `/crossnook` with Storage Layout children created
during the successful composition gate.

The existing boot FAT32 filesystem is only an initial focused portable target:
mode bits confer no meaningful confidentiality there; directory-fsync and
power-loss durability require physical evaluation. Boot files and mutable
state share the FAT filesystem. Future credentials must not inherit a storage
security policy from this milestone. No partitioning or new data filesystem
is selected here.

## Lifecycle limitation

The platform's startup/deployment layer must own the mount lifecycle and
prevent unmount/replacement while CrossNook persists. Pathname checks do not
prevent a concurrent unmount/replacement between verification and an existing
store's later pathname-based writes; this is not descriptor-relative
containment. A bind mount of an entire filesystem root can also be
indistinguishable from a primary mount using these kernel interfaces; trusted
deployment must not supply one. Failed verification never returns a fallback.
