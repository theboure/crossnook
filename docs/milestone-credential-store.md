# Credential Store Core

**Status: IMPLEMENTATION COMPLETE; HOST VALIDATION PASS; HARDWARE VALIDATION PASS.**

## Boundary and API

`src/credentials/credential_store.[ch]` persists exactly the username and
already-derived KOSync userkey sent as `x-auth-user` and `x-auth-key`. The key
is an account-equivalent secret; this module does not take or hash a password,
fetch a token, authenticate to a server, or select a storage root. The fixed
arrays contain at most 255 bytes per field and are owned by the caller.

```c
void cn_credentials_clear(cn_credentials *credentials);
cn_credential_result cn_credential_store_init(cn_credential_store *store,
                                               const char *directory,
                                               int *system_errno);
cn_credential_result cn_credential_store_load(const cn_credential_store *store,
                                               cn_credentials *credentials,
                                               int *system_errno);
cn_credential_result cn_credential_store_save(const cn_credential_store *store,
                                               const cn_credentials *credentials,
                                               int *system_errno);
const char *cn_credential_result_name(cn_credential_result result);
```

The explicit absolute directory must exist; store initialization copies it
without creating anything. The single final filename is `credentials`. Only
startup composition may derive `<verified-root>/state` after successful
portable-storage verification and Storage Layout preparation. The standalone
store deliberately does not inspect mounts, `/sdcard`, or `/data`.

Missing loads do not create a file. All non-OK loads zero **every byte** of the
output, including partially parsed material. Corruption and unsupported-version
loads never write or repair a file. Only an explicit save replaces one. A
missing credential means sync cannot authenticate, even if non-secret Settings
enable sync. Neither the Settings schema nor Storage Layout was changed.

## Exact version 1 format

```text
crossnook-credentials=1
username=<bounded nonempty header value>
userkey=<bounded nonempty header value>
crc32=<eight lowercase hex digits>
```

The final newline is mandatory. The CRC32 is IEEE over all preceding bytes,
including the newline immediately before `crc32`. It detects accidental
corruption, **not** tampering or authentication. At most 1024 bytes total are
read; trailing data, omitted or duplicate fields, reordered/unknown fields,
embedded NUL, incorrect CRC, invalid field lengths, and noncanonical newline
or version syntax are corrupt. Canonical decimal versions other than `1`
return `UNSUPPORTED_VERSION`. Values follow the existing KOSync client header
rule: nonempty, up to 255 bytes, rejecting C0 and DEL; all other accepted bytes
are preserved exactly. In particular, userkeys are not constrained to 32 hex
digits. Do not place a credential in a URL, device name, filename, or Settings.

## Filesystem and durability policy

The directory must be an existing non-symlink final component; final file
symlinks and non-regular entries (including directories, FIFOs, and devices)
are refused. Load uses `lstat`, `O_NOFOLLOW`, `O_NONBLOCK`, `fstat`, and
device/inode checks; bounded reads reject truncation and appended bytes.
Paths and directory ancestry are not an adversarial sandbox, and concurrent
path/mount replacement remains outside this single-user deployment contract.

Save creates a same-directory uniquely named temporary file using `O_EXCL`
and `O_NOFOLLOW`, writes fully, syncs and checks close, then renames over the
regular/missing final entry. Pre-rename failures leave the prior final entry
intact; only the temporary file created by that call is removed. A crash can
leave an orphaned temporary file containing secret material. After rename,
failure to open/sync/close the directory yields `DURABILITY_UNCERTAIN`: the new
file is visible **now**, but persistence across power loss is unconfirmed.
The caller must inspect/reload, not blindly retry or roll back. The runtime
policy depends on observed syscall results, not filesystem-name guesses.
Success means the file and directory sync calls succeeded, **not** a blanket
power-loss guarantee on old FAT32 media. Directory `fsync()` may return an
unsupported error such as `EINVAL`, which is reported as uncertain if rename
already succeeded. Actual FAT32 rename/directory durability needs physical
measurement; ext-filesystem semantics cannot be assumed.

## Secret and FAT32 limitations

The store logs nothing and offers no secret-bearing debug API. Diagnostics use
only built-in synthetic fixtures, accept no credential argv values, and output
statuses without values. Temporary parsing/serialization arrays and returned
failure outputs are cleared using volatile byte writes; callers must clear
successful outputs and transient client copies at end of use. This hygiene
cannot erase prior transport copies, inspectable process/kernel memory, or
filesystem history, and cannot protect against malicious root.

The current FAT32 boot microSD has no meaningful Unix-mode confidentiality:
`0600` is only hygiene. Offline inspection or physical removal can recover a
credential; a sufficiently privileged running process can read it. Root can
read the filesystem and memory. Replacement/deletion is not secure erasure;
backups and card images may retain credentials. No encryption or device-binding
key is provided. A key stored beside ciphertext on the card would not improve
protection against card removal. No stable device identity is required for
KOSync authentication. Do not store real credentials in test fixtures or logs.

## Host validation

`bash testapp/build-credentials.sh` cross-compiles a static ARMv5TE EABI5
soft-float non-PIE diagnostic and runs synthetic tests under QEMU. The tests
exercise missing, deterministic save/exact reload, malformed/version/oversize
cases, symlinks and non-regular entries, pre-rename injected write/file-sync/
close/rename errors with old-file preservation, post-rename directory sync and
close uncertainty with new-file visibility, and sentinel/redaction/argv gates.
The `--smoke` mode accepts only a host-test `/tmp/crossnook-credentials-host.*`
directory; physical operations use `--physical` and verify storage first.
`--check-output </tmp/log>` scans a bounded diagnostic capture for all built-in
synthetic values and prints only `redaction=ok` or `redaction=failed`. Its host
gate confirms both detection of a deliberately contaminated fixture and no
secret-bearing output from the scanner itself.

```text
CREDENTIAL STORE SMOKE failures=0 -> OK
CREDENTIAL STORE HOST VALIDATION OK
```

## Physical hardware validation

The physical Nook gate used only `testapp/crossnook-credentials-test` and its
internally fixed synthetic values. The operator confirmed `/dev/block/mmcblk1`
(`179:16`) as the external microSD and mounted it at `/tmp/crossnook-card`.
The published verifier accepted `/tmp/crossnook-card/crossnook`:

```text
STORAGE VERIFY result=ok errno=0 root=/tmp/crossnook-card/crossnook mount=/tmp/crossnook-card device=179:16
```

No real KOSync credential was used or entered through PowerShell or adb; no
`/data` path or intentional internal eMMC write was used. Executable and
stdout/stderr captures remained under `/tmp`.

The physical gate used an independently provisioned application root and
`state` directory. Its operator procedure was:

From the PC, deploy only the ARM executable to `/tmp` with `adb push
testapp/crossnook-credentials-test /tmp/crossnook-credentials-test`, then make
that `/tmp` copy executable. In the Nook shell, after independent card identity
recon and with the published storage verifier also deployed under `/tmp`:

```sh
D=/tmp/crossnook-credentials-test
V=/tmp/crossnook-storage-verify-test
M=/tmp/crossnook-card
R=/tmp/crossnook-card/crossnook
L=/tmp/crossnook-credential-gate.log
printf '%s\n' 'synthetic-sentinel' > /tmp/crossnook-credential-sentinel

# Unmounted negative gate: expected nonzero, no persistence attempted.
"$D" --physical missing "$M" "$R" 179 16 > "$L" 2>&1
"$D" --check-output "$L"

# Only after reconfirming mmcblk1 is the external boot card:
mkdir /tmp/crossnook-card
mount -t vfat -o rw /dev/block/mmcblk1 "$M"
cat /proc/mounts
cat /proc/self/mountinfo
"$V" --verify "$M" "$R" 179 16
ls -ld "$R/state"

# Stop if verification or the existing state directory check failed.
# Missing must not create credentials. Save refuses an existing credential.
"$D" --physical missing "$M" "$R" 179 16 >> "$L" 2>&1
"$D" --physical save "$M" "$R" 179 16 >> "$L" 2>&1
"$D" --physical load "$M" "$R" 179 16 >> "$L" 2>&1
"$D" --physical corrupt "$M" "$R" 179 16 >> "$L" 2>&1
"$D" --check-output "$L"
cat /tmp/crossnook-credential-sentinel

# With no diagnostic running, unmount; expected nonzero, no persistence.
umount "$M"
"$D" --physical missing "$M" "$R" 179 16 >> "$L" 2>&1
"$D" --check-output "$L"
```

Operator must inspect each result and stop on unexpected status; the shell
sequence is a plan, not an automatic unattended script. The `load` phase is a
fresh process comparing fixture values internally; `corrupt` preserves the
damaged file through its failed load. Record `save=durability-uncertain` if the
card's directory sync fails, rather than treating the result as an ext-like
success. Compare the outside-root sentinel with its initial bytes and inspect
write scope on the external card. No `/data` or internal eMMC storage path is
selected; no real KOSync credential enters the transcript.

The reported gate output was:

```text
# Before mounting and again after unmounting:
CREDENTIAL GATE storage=unverified persistence=not-attempted
CREDENTIAL GATE redaction=ok

# Mounted verified microSD:
CREDENTIAL GATE credential=missing cleared=yes
CREDENTIAL GATE save=ok
CREDENTIAL GATE reload=ok
CREDENTIAL GATE redaction=ok
CREDENTIAL GATE corrupt=corrupt cleared-and-preserved=yes
CREDENTIAL GATE redaction=ok
```

The fresh-process reload compared only internal synthetic values. The corrupt
record yielded cleared output and was preserved, without automatic rewriting.
The outside-root sentinel remained `synthetic-sentinel`. The two unmounted
gates reported no persistence attempt through the underlying directory.

A direct remote-shell check confirmed the diagnostic's exit status for the
unmounted negative gate:

```text
CREDENTIAL GATE storage=unverified persistence=not-attempted
REMOTE_RC=1
```

The nonzero remote status is expected: storage verification rejected the
unmounted candidate and persistence was not attempted. An earlier PowerShell
`$LASTEXITCODE=0` reflected the host `adb` invocation, **not** the remote
diagnostic exit status.

On this Nook's old-kernel/vfat filesystem, the focused save returned `ok`, not
`durability-uncertain`. This is evidence for that run's successful syscall path,
**not** a general guarantee of FAT32 atomicity or power-loss persistence.
