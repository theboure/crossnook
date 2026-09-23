# Persistent Settings Core

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

This milestone adds bounded, versioned persistence for three non-secret
application settings. It has no UI, sync controller, credential handling,
mount discovery, or storage-root policy.

## V1 Schema

`src/settings/settings_store.[ch]` defines one fixed-size settings object:

```c
typedef struct cn_settings {
    int kosync_enabled;
    char kosync_base_url[CN_SETTINGS_KOSYNC_URL_CAPACITY];
    char kosync_device_name[CN_SETTINGS_DEVICE_NAME_CAPACITY];
} cn_settings;
```

Exact defaults are:

```text
kosync_enabled = false
kosync_base_url = ""
kosync_device_name = "CrossNook"
```

KOSync defaults disabled. An empty base URL is valid only while it is disabled.
A nonempty URL must use `https://`, fit the existing KOSync host, port, and
base-path bounds, and contain no userinfo, query, fragment, whitespace, control
character, CR, LF, NUL, or non-ASCII byte. The future controller must still
call `cn_kosync_client_init()` before network activity; Settings Store does not
duplicate the complete transport parser.

Device names are nonempty strict UTF-8, at most 255 bytes, with no C0, DEL, or
C1 control code point. Values are rejected rather than truncated.

The schema contains no username, key, token, password, PSK, cookie, private
key, encryption key, device ID, sync trigger, Reader option, books directory,
DNS/SNTP setting, Wi-Fi behavior, CA/entropy path, storage root, cache, logging,
or boot configuration. Credentials require a separate threat model. Mode
`0600`, especially on removable media, is not a confidentiality guarantee.

## Public API

```c
void cn_settings_defaults(cn_settings *settings);

cn_settings_result cn_settings_validate(
    const cn_settings *settings);

cn_settings_result cn_settings_store_init(
    cn_settings_store *store,
    const char *config_directory,
    int *system_errno);

cn_settings_result cn_settings_load(
    const cn_settings_store *store,
    cn_settings *settings,
    int *system_errno);

cn_settings_result cn_settings_save(
    const cn_settings_store *store,
    const cn_settings *settings,
    int *system_errno);
```

`cn_settings` and `cn_settings_store` own fixed arrays. The module allocates no
heap memory, retains no caller pointer, and needs no close operation.
`system_errno` is optional, is reset on API entry, and captures syscall failure
values immediately.

Results distinguish success, missing settings, invalid arguments, invalid
settings, corrupt input, unsupported versions, path length, missing CONFIG,
non-directory CONFIG, symlinks, non-regular settings entries, permission
denial, read-only storage, no space/quota, other I/O, and uncertain durability.

## Storage Composition

The caller supplies one existing CONFIG directory. The final file is:

```text
<config-directory>/settings.conf
```

Settings Store never creates CONFIG and has no dependency on
`storage_layout.h`. A future startup layer composes the modules:

```text
caller-selected application root
-> cn_storage_layout_init()
-> cn_storage_layout_prepare()
-> cn_storage_layout_path(CONFIG)
-> cn_settings_store_init()
-> cn_settings_load()
```

The store discovers no mount or application root and selects no `/tmp`,
`/data`, `/sdcard`, or `/mnt/*` path. In the current NookManager environment,
`/data` is internal eMMC and is not a default.

The supplied CONFIG path is copied. It must be absolute and already name a real
directory rather than a final-component symlink. Trailing slashes are removed.
Ancestor symlinks remain allowed under the trusted single-user appliance threat
model.

## File Format

V1 is strict deterministic UTF-8 text:

```text
crossnook-settings=1
kosync.enabled=false
kosync.base_url=
kosync.device_name=CrossNook
crc32=01234567
```

The CRC32 is the IEEE CRC over every byte through the newline immediately
before `crc32`. It detects accidental corruption only. It provides no
authentication, integrity against intentional edits, or confidentiality.

The file is limited to 2048 bytes. Field order is exact; every field appears
exactly once; a final newline is required. Comments, blank lines, unknown keys,
duplicates, missing fields, malformed lines, noncanonical versions, invalid
booleans, embedded NUL, malformed UTF-8, invalid values, bad CRC, and trailing
bytes are corrupt. Boolean values are exactly `true` or `false`; CRC text is
exactly eight lowercase hexadecimal digits. There is no escaping syntax.

Version 1 has one schema. A syntactically valid version other than 1 returns
`UNSUPPORTED_VERSION`. Unknown v1 fields are not ignored. There is no migration
for nonexistent historical formats.

## File-Type Policy

Before load, `settings.conf` is inspected with `lstat()`. Only a regular,
non-symlink final entry is opened. Directories, FIFOs, sockets, character and
block devices, and other non-regular entries return `NON_REGULAR`; a final
symlink returns `SYMLINK`. `O_NOFOLLOW`, `fstat()`, and device/inode comparison
add checks around open without claiming an adversarial descriptor-relative
sandbox.

Save applies the same final-entry policy before replacement. Atomic rename
would replace rather than follow a symlink, but an observed final symlink or
non-regular entry is rejected explicitly. Normal check/use races and ancestor
path replacement remain outside this trusted-appliance threat model.

## Load Semantics

Load starts with exact defaults and parses into a temporary object:

| Condition | Result | Caller output |
|---|---|---|
| Missing file | `MISSING` | exact defaults |
| Valid v1 | `OK` | fully validated file values |
| Invalid current v1 | `CORRUPT` | exact defaults |
| Other version | `UNSUPPORTED_VERSION` | exact defaults |
| Filesystem failure | specific error | exact defaults |

Invalid API arguments have no output guarantee. No partial parse is exposed.
Missing settings do not create a file. Corrupt settings are not removed or
overwritten automatically; the caller receives defaults and the explicit
error, and decides how to report or recover.

## Atomic Save And Durability

Save validates and serializes the complete object in bounded memory, writes
`settings.conf.tmp.<pid>` in the same directory with `O_EXCL` and requested
mode `0600`, writes all bytes, synchronizes and closes the temporary file, then
atomically renames it over `settings.conf`. It opens and synchronizes CONFIG
after rename. Pre-rename failures preserve the previous settings file and
attempt to remove the temporary file; a cleanup failure is itself returned
explicitly.

`EACCES` and `EPERM` map to `PERMISSION_DENIED`; `EROFS` maps to `READ_ONLY`;
`ENOSPC` and `EDQUOT` map to `NO_SPACE`.

If CONFIG open, `fsync()`, or close fails after rename, save returns
`DURABILITY_UNCERTAIN`. The new file is already the logically current visible
value and is not rolled back. Only persistence across sudden power loss is
uncertain. There is no journal, generation history, locking, or multi-writer
support.

## Host Gate

Run only the focused gate:

```bash
bash testapp/build-settings.sh
```

It builds a static ARM EABI5 soft-float non-PIE diagnostic, verifies the
production settings object has no heap references, checks the public schema for
excluded fields, and runs the diagnostic under QEMU. Test-only linker wrapping
injects ENOSPC, EDQUOT, temporary-file `fsync`, close, rename, and CONFIG
directory `fsync` failures without production hooks.

Observed result:

```text
SETTINGS STORE SMOKE failures=0 -> OK
SETTINGS SAVE result=permission-denied errno=13
SETTINGS STORE HOST VALIDATION OK
```

The matrix covers defaults, missing/corrupt/unsupported behavior, strict
format and CRC, bounds, URL and UTF-8 validation, deterministic repeated saves,
regular-file and symlink policy, stale temporary cleanup, every injected save
stage, preservation of the old file before rename, authoritative new content
after durability uncertainty, concurrent old-or-new visibility, non-root
permission denial, read-only/protected storage, Storage Layout CONFIG
composition, outside-root isolation, result bounds, and absence of serialized
credential fields.

## Physical Gate

Hardware validation passed on the physical Nook using only
`/tmp/crossnook-settings-root`. No `/data` path was used, no internal eMMC
write was intentionally performed, and no microSD mountpoint was guessed.

Validated missing path:

```text
[OK] physical Storage Layout CONFIG composes
[OK] physical missing settings returns exact defaults without creation
[OK] physical outside sentinel unchanged
SETTINGS physical-missing config=/tmp/crossnook-settings-root/config result=missing defaults=ok
```

Validated save:

```text
[OK] physical save composes CONFIG
[OK] physical non-secret settings save
[OK] physical repeated save is deterministic
[OK] physical outside sentinel unchanged
```

Validated fresh-process load:

```text
[OK] physical reload composes CONFIG
[OK] physical fresh invocation reloads exact settings
[OK] physical outside sentinel unchanged
```

Loaded values:

```text
kosync_enabled=1
kosync_base_url=https://sync.example.test/base
kosync_device_name=CrossNook Physical Test
```

Validated corruption behavior:

```text
[OK] physical corruption composes CONFIG
[OK] physical saved file captured
[OK] physical corruption returns exact defaults
[OK] physical corrupt file is not overwritten
[OK] physical outside sentinel unchanged
SETTINGS physical-corrupt result=corrupt defaults=ok preserved=ok
```

The reproducible physical validation procedure is below. From PowerShell,
deploy and run the ARM diagnostic using only disposable `/tmp` paths:

```powershell
$ErrorActionPreference = 'Stop'
$Adb = 'C:\platform-tools\adb.exe'
function Invoke-Adb {
    & $Adb @args
    if ($LASTEXITCODE -ne 0) { throw "adb failed: $args" }
}

bash testapp/build-settings.sh
if ($LASTEXITCODE -ne 0) { throw 'focused settings build failed' }

Invoke-Adb push .\testapp\crossnook-settings-test /tmp/crossnook-settings-test
Invoke-Adb shell 'chmod 755 /tmp/crossnook-settings-test'
Invoke-Adb shell 'set -e; rm -rf /tmp/crossnook-settings-root /tmp/crossnook-settings-outside && mkdir /tmp/crossnook-settings-root && printf "outside\n" > /tmp/crossnook-settings-outside'

Invoke-Adb shell '/tmp/crossnook-settings-test --physical-missing /tmp/crossnook-settings-root /tmp/crossnook-settings-outside'
Invoke-Adb shell '/tmp/crossnook-settings-test --physical-save /tmp/crossnook-settings-root /tmp/crossnook-settings-outside https://sync.example.test/base "CrossNook Physical Test"'
Invoke-Adb shell '/tmp/crossnook-settings-test --physical-load /tmp/crossnook-settings-root /tmp/crossnook-settings-outside https://sync.example.test/base "CrossNook Physical Test"'
Invoke-Adb shell '/tmp/crossnook-settings-test --physical-corrupt /tmp/crossnook-settings-root /tmp/crossnook-settings-outside'

Invoke-Adb shell 'rm -rf /tmp/crossnook-settings-root /tmp/crossnook-settings-outside /tmp/crossnook-settings-test'
```

The commands prepare Storage Layout, derive CONFIG, prove missing defaults
without file creation, save only non-secret values, verify byte-identical
repeated serialization, reload in a fresh process, verify corruption produces
defaults without overwriting the corrupt bytes, and check the outside sentinel
at every phase. They do not use `/data`, guess a microSD mountpoint, or claim
persistence across reboot.
