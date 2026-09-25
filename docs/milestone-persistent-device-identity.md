# Persistent Device Identity Core

This milestone adds a standalone, installation-scoped CrossNook device
identity. It does not wire the identity into KOSync, normal sync, explicit
push/pull, Reader sync, conflict handling, settings, startup, or existing
diagnostics. Those callers continue to use their existing explicit runtime
IDs.

## Scope and Boundary

The identity belongs to the CrossNook installation/storage root. The caller
must first select and positively verify the external storage, initialize the
existing `cn_storage_layout`, and call `cn_storage_layout_prepare()`. The
identity module does not discover mounts, verify mountpoints, create the root,
or create layout directories.

The file is stored at:

```text
<verified-root>/config/device-id
```

The module operates only on that prepared layout. It never touches internal
eMMC and does not derive identity from MAC addresses, serial numbers, eMMC
identifiers, Wi-Fi state, timestamps, PID, or other hardware/runtime data.

The identity is not an account secret. It is an installation identifier and
is persisted with mode `0600` as a conservative filesystem default. Copying a
CrossNook tree copies its identity by design.

## Identity and File Format

Creation reads exactly 16 bytes and encodes them as exactly 32 lowercase
hexadecimal ASCII characters. This is within the existing KOSync printable
UTF-8 and 255-byte device-ID constraints without changing KOSync.

The persisted record is exactly:

```text
crossnook-device-identity=1
id=<32 lowercase hex chars>
crc32=<8 lowercase hex chars>
```

The CRC32 covers the complete first two lines including their terminating
newlines. Parsing rejects embedded NULs, missing or extra lines, malformed
version/ID/checksum fields, and trailing data. Unsupported versions and bad
checksums are explicit typed failures.

An existing malformed, corrupt, or unsupported identity is a hard failure.
The module never silently rotates or replaces it. There is no automatic
identity recovery or rotation in this milestone.

## Entropy

The API accepts an injectable entropy callback for deterministic tests. A
NULL callback uses the production source `/dev/urandom`, reading exactly 16
bytes and retrying interrupted reads. Open, read, short-read, and close
failures return `entropy-failed`; there is no timestamp, PID, hardware, or
deterministic fallback. This does not claim stronger entropy guarantees than
the existing Nook Linux environment provides.

## Creation and Durability

The module first loads the existing final file. Only a genuine absence causes
entropy generation. First creation uses an exclusive final-file create,
writes the complete bounded record, fsyncs the file, and fsyncs the containing
directory using the existing project convention. If another creator wins the
exclusive create, the winner is reopened and strictly validated; it is never
overwritten.

Write failures clean up partial files created by the current process where
safe. A file that becomes visible while final durability cannot be confirmed
returns `durability-uncertain`. FAT32 power-loss behavior remains limited;
this module does not claim transactional durability. A torn visible file is
reported as malformed or checksum-invalid on the next open rather than being
replaced.

## Validation

The focused host matrix covers first creation, stable reopen, distinct
injected IDs for separate roots, malformed/truncated data, unsupported
version, bad CRC, entropy failure, insufficient output capacity, invalid
arguments, missing config directories, valid-existing-file loads without
entropy or rewrite, and a deterministic exclusive-create winner race.

The diagnostic links no network or KOSync code and does not invoke storage
verification in the identity module. The ARM artifact is built static,
non-PIE for ARMv5TE EABI5 soft-float and runs the same smoke matrix under
QEMU.

## Physical Diagnostic

The identity-only physical mode is:

```text
/tmp/crossnook-device-identity-test --physical <mountpoint> <root> <major> <minor>
```

The caller supplies the mountpoint, application root, and expected device
numbers. With the card unmounted, the diagnostic verifies first and reports
`storage=unverified persistence=not-attempted network=not-attempted` with a
nonzero exit. It does not access identity storage.

After mounting and verifying the external card, prepare the layout and run it
twice. The first run reports `created`; the second reports `ok`, the same
comparison digest, and `existing-unchanged=yes`. Check an outside-root
sentinel before and after both runs. Unmount and repeat the refusal. No
credentials, DNS, TLS, trusted time, KOSync, or other networking is required.

## Deferred Work

Loading this identity into normal sync configuration, replacing synthetic
diagnostic IDs, account/device migration, identity backup/export, rotation,
recovery tooling, hardware binding, and multi-installation policy are all
deferred to later milestones.
