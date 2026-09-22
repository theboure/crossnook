# Portable Storage Layout Core

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

This milestone adds a bounded, allocation-free layout for application-owned
configuration and reading progress. The caller supplies the application root;
the module does not discover storage, select a mountpoint, or guess whether a
filesystem is internal or removable.

## Public API

`src/storage/storage_layout.[ch]` exports:

```c
cn_storage_result cn_storage_layout_init(
    cn_storage_layout *layout,
    const char *root,
    int *system_errno);

cn_storage_result cn_storage_layout_path(
    const cn_storage_layout *layout,
    cn_storage_location location,
    char *out,
    size_t capacity);

cn_storage_result cn_storage_layout_prepare(
    const cn_storage_layout *layout,
    int *system_errno);
```

`cn_storage_layout` owns a fixed-size copy of the normalized root. The API does
not allocate memory or retain caller pointers. Derived paths are written into
caller-owned buffers and never silently truncated.

## Layout

The two public locations are:

| Location | Derived path |
|---|---|
| `CN_STORAGE_LOCATION_CONFIG` | `<root>/config` |
| `CN_STORAGE_LOCATION_PROGRESS` | `<root>/state/progress` |

`<root>/state` is an internal parent and is not a public location. Books,
caches, logs, temporary files, and credentials are outside this contract.

The root must already exist as a directory. It must be absolute, cannot be
filesystem root `/`, and cannot contain empty, `.` or `..` components. Trailing
slashes are removed; no other lexical or physical canonicalization is done.
The final root entry must not be a symbolic link. Ancestor symlinks are allowed,
so this is a predictable layout API rather than an adversarial filesystem
sandbox.

The longest derived path is bounded to 4095 bytes including all bytes before
the terminating NUL. Roots that cannot produce `<root>/state/progress` within
that limit are rejected during initialization.

## Preparation

`cn_storage_layout_prepare()` creates directories in this order with requested
mode `0700`:

1. `<root>/state`
2. `<root>/state/progress`
3. `<root>/config`

Preparation is idempotent and does not change permissions on existing
directories. An existing non-directory or final managed-child symlink is
rejected. If `mkdir()` reports `EEXIST`, the entry is inspected again before it
is accepted, covering a concurrent creator without treating the API as a
security boundary.

Results distinguish invalid input, excessive path length, insufficient output
space, missing entries, non-directories, symlinks, permission denial, read-only
filesystems, creation failure, and other I/O failure. The optional
`system_errno` output is reset on entry and captures a failing syscall's
`errno`; semantic validation failures leave it zero.

## ProgressStore Composition

The `PROGRESS` path is passed directly to the existing
`cn_progress_store_open()` API after successful preparation. ProgressStore
continues to own record serialization and atomic file replacement. Neither
module discovers or defaults a device path, and this milestone does not modify
Reader, ProgressStore, Library, Book Identity, or KOSync behavior.

No removable microSD mountpoint has been verified for the current device
environment. `/data` is internal eMMC and must not be treated as removable
storage. A future application/controller must obtain its chosen root from an
explicit deployment or user configuration boundary.

## Host Gate

Run:

```bash
bash testapp/build-storage-layout.sh
```

The gate builds `testapp/crossnook-storage-layout-test` as a static ARM EABI5
soft-float non-PIE executable and runs it under QEMU. The smoke matrix covers
root validation and ownership, trailing-slash normalization, exact derived
paths, output boundaries, maximum path length, missing and non-directory roots,
final symlinks, reinitialization, ordered and repeated preparation, file
conflicts, managed-child symlinks, read-only/protected storage, an actual
non-root permission-denied attempt, outside-sentinel isolation, enum bounds,
and opening the derived progress path with ProgressStore.

Observed result:

```text
STORAGE LAYOUT API SMOKE failures=0 -> OK
STORAGE PREPARE result=permission-denied errno=13
STORAGE LAYOUT HOST VALIDATION OK
```

## Hardware Gate

Hardware validation used only `/tmp/crossnook-storage-layout`. No `/data` path
was used, no internal eMMC write was intentionally performed, and no microSD
mountpoint was guessed. The outside sentinel fixture contained exactly
`outside\n`.

Final physical run:

```text
[OK] physical root initializes
[OK] physical trailing slash normalizes
[OK] physical CONFIG derives
[OK] physical PROGRESS derives
[OK] physical prepare succeeds
[OK] physical repeated prepare succeeds
[OK] physical directories exist
[OK] physical progress sentinel round-trips
[OK] physical ProgressStore opens
[OK] physical outside sentinel remains unchanged

STORAGE root=/tmp/crossnook-storage-layout
config=/tmp/crossnook-storage-layout/config
progress=/tmp/crossnook-storage-layout/state/progress
prepare=ok
repeat=ok
isolation=ok
STORAGE LAYOUT SMOKE failures=0 -> OK
```

The reproducible physical validation command is:

```sh
rm -rf /tmp/crossnook-storage-layout /tmp/crossnook-storage-layout.outside
mkdir /tmp/crossnook-storage-layout
printf 'outside\n' > /tmp/crossnook-storage-layout.outside
./crossnook-storage-layout-test --physical \
  /tmp/crossnook-storage-layout \
  /tmp/crossnook-storage-layout.outside
./crossnook-storage-layout-test --physical \
  /tmp/crossnook-storage-layout \
  /tmp/crossnook-storage-layout.outside
```

Both runs must report exact CONFIG and PROGRESS paths, successful preparation,
real managed directories, successful ProgressStore opening, and an unchanged
outside sentinel.

An earlier failed hardware run used a different manually created sentinel
payload. That was a fixture mismatch, not a storage-core defect. The final
hardware run above is authoritative.
