# Milestone: Local Reading Progress Persistence

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.** Local logical
reading progress survives Reader and application process lifetime. No network,
KOSync, settings, progress UI, or physical-page restore was added.

## Architecture

The dependency boundary is:

```
CREngine
    -> cn_reader_position (canonical logical location)
    -> cn_progress_record
    -> cn_progress_store (local files)
    -> future SyncProvider
    -> future KOSyncClient
```

- `src/reader/reader.cpp` remains the only layer that interprets CREngine
  XPointers.
- `src/progress/book_identity.{h,c}` derives an opaque book identity.
- `src/progress/progress_store.{h,c}` owns serialization and filesystem I/O.
- `src/ui/ui.c` only forwards capture/restore calls to its owned Reader. It
  contains no persistence policy or filesystem access.
- `src/app/reader-test.c` composes Library, Reader, identity, and ProgressStore
  and decides when to save or restore.
- `src/library/library.c` is unchanged.

The state directory is mandatory in device mode:

```
crossnook-reader-test <font.ttf> <books-dir> <state-dir>
```

It must already exist. There is no default and no hardcoded `/data`, `/home`,
or other device path. Failure to open or write the store is logged and does
not prevent normal reading.

## Book Identity

The initial identity is `path-v1-` followed by the lowercase 64-bit FNV-1a
digest of the exact path bytes passed to Reader. The fixed ASCII token is safe
as a filename and prevents a book path from introducing `/`, `..`, or other
path traversal into the state directory. Identity generation is isolated
behind `cn_book_identity_from_path()`; ProgressStore callers treat the token as
opaque.

This is deliberately not KOReader/KOSync document identification and is not a
content hash. Moving or renaming a book, changing the lexical path, or mounting
the same file elsewhere creates a different identity. Replacing content at the
same path inherits the old identity, and 64-bit hash collision is theoretically
possible. The Reader still rejects a saved canonical position that no longer
resolves, but structurally compatible replacement content can be
indistinguishable. A future content-derived or KOSync-compatible identity can
replace/extend `path-v1` without changing ReaderPosition, UI, or the
ProgressStore API.

## Format Version 1

Each book has one `<identity>.progress` file. All integers are unsigned
big-endian 32-bit values unless noted:

| Bytes | Value |
| --- | --- |
| 8 | magic `CNPROG\r\n` |
| 4 | format version (`1`) |
| 4 | identity byte length |
| 4 | logical location byte length |
| 4 | normalized progress (`0..10000`; `0xffffffff` means unknown) |
| variable | opaque ASCII identity token |
| variable | canonical UTF-8 ReaderPosition location |
| 4 | IEEE CRC32 of all preceding bytes |

The identity is at most 24 bytes and the location is at most 65,536 bytes, so
the complete v1 record is bounded at 65,588 bytes. Loading requires exact file
length, matching identity, valid CRC, valid progress, a nonempty slash-prefixed
location, and strict UTF-8. Embedded NUL, malformed/truncated input, bad CRC,
oversized fields, and unsupported versions are rejected before a
ReaderPosition is returned. ProgressStore does not parse XPointer syntax;
`cn_reader_goto_position()` remains the canonical validator against the open
document.

## Atomic Writes

Save serializes a complete bounded record in memory, creates a same-directory
`<identity>.progress.tmp.<pid>` file with mode `0600`, writes all bytes, calls
`fsync()`, checks `close()`, and atomically renames it over the destination.
Failure before rename removes the temporary file and leaves the last good
record untouched. Directory `fsync()` is attempted after rename; filesystems
that reject directory synchronization still retain atomic complete-record
replacement, but power-loss durability remains filesystem-dependent and is a
hardware validation concern.

The current application is single-process. ProgressStore does not yet lock
against concurrent writers.

## Save And Restore Policy

The application saves the current logical position:

- immediately before BACK leaves Reader;
- immediately before HOME leaves Reader;
- during orderly application cleanup while Reader is still open, including a
  long-POWER exit.

PAGE_NEXT/PAGE_PREV do not write state. This keeps policy deterministic and
avoids unnecessary flash writes. Save/capture errors are concise stderr
diagnostics and never block navigation or exit.

After a successful EPUB activation, the application performs:

```
reader_open(book.path)
identity_from_path(book.path)
progress_store_load(identity)
cn_reader_goto_position(saved logical location)
render
```

A missing record leaves the new Reader at page 0. Corrupt, unsupported,
unreadable, noncanonical, or document-invalid state is rejected, also leaving
the usable Reader at page 0. Normal diagnostics expose only opaque identity
tokens and result names, never raw XPointers.

## Host Validation

`bash testapp/build-progress.sh` builds a static ARM EABI5 non-PIE binary and
runs three QEMU phases:

1. bounded format/error tests for missing, valid, truncated, malformed,
   unsupported-version, invalid-UTF-8, oversized, invalid-progress, unwritable,
   and atomic-overwrite cases;
2. process A saves independent positions for books A and B, overwrites B, and
   exits;
3. process B opens the same explicit state directory, restores both books,
   rejects a disk-loaded position against structurally different content, and
   proves relayout restoration through the persisted token.

Observed relayout result:

```
PROGRESS relayout pages=79->254 page=26->82 progress=3210
PROGRESS PROCESS RESTART failures=0 -> OK
```

The complete host gate is:

```
bash testapp/build-progress.sh
bash testapp/build-position.sh
bash testapp/build-test.sh
bash testapp/host-live-test.sh
bash testapp/build-text.sh
python testapp/validate-text.py testapp/fb-text.bin
bash testapp/build-ui.sh
bash testapp/build-library.sh
bash testapp/build-cre.sh
bash testapp/build-reader.sh
bash toolchain/build-smoke.sh
```

Existing golden expectations are unchanged.

## Hardware Validation

Real-Nook validation passed. Confirmed behavior:

- local logical progress persists across process restart;
- `valid.epub` restores its saved logical position;
- `valid2.epub` maintains an independent saved position;
- BACK, HOME, and clean long-POWER exit while Reader is open save progress;
- persisted logical positions remain valid across relayout;
- corrupt or missing state does not crash or blank the Reader; and
- normal NEXT/PREV/BACK/HOME behavior remains correct.

During validation, intermittent false touch coordinates near Y=0 were traced
with raw `/dev/input/event2` logs to the Nook zForce hardware stream, not to
CrossNook input processing. Cleaning dust and debris from the display bezel and
IR sensor area eliminated the symptom. Treat recurrence as a hardware
maintenance issue; no software touch-filter workaround is included.

## Real-Nook Validation Procedure

Run these commands from PowerShell at the repository root. All state remains
under `/tmp`; this milestone never uses `/data`.

1. Build the two required ARM binaries:

   ```powershell
   bash testapp/build-progress.sh
   bash testapp/build-reader.sh
   ```

2. Confirm the device, then reset temporary device directories and push
   binaries and fixtures:

   ```powershell
   C:\platform-tools\adb.exe devices
   C:\platform-tools\adb.exe shell "rm -rf /tmp/crossnook-progress-errors /tmp/crossnook-progress-state /tmp/crossnook-app-state /tmp/crossnook-progress-books /tmp/reader-fixtures; mkdir -p /tmp/crossnook-progress-errors /tmp/crossnook-progress-state /tmp/crossnook-app-state /tmp/crossnook-progress-books /tmp/reader-fixtures"
   C:\platform-tools\adb.exe push testapp/crossnook-progress-test /tmp/
   C:\platform-tools\adb.exe push testapp/crossnook-reader-test /tmp/
   C:\platform-tools\adb.exe push testapp/test-font.ttf /tmp/
   C:\platform-tools\adb.exe push testapp/cre-fixtures/test.epub /tmp/crossnook-progress-books/test.epub
   C:\platform-tools\adb.exe push testapp/reader-fixtures/valid2.epub /tmp/crossnook-progress-books/valid2.epub
   C:\platform-tools\adb.exe push testapp/position-fixtures/foreign.epub /tmp/crossnook-progress-books/foreign.epub
   C:\platform-tools\adb.exe push testapp/reader-fixtures/. /tmp/reader-fixtures/
   C:\platform-tools\adb.exe shell "chmod 755 /tmp/crossnook-progress-test /tmp/crossnook-reader-test"
   ```

3. Run format/corruption validation on-device:

   ```powershell
   C:\platform-tools\adb.exe shell "/tmp/crossnook-progress-test --errors /tmp/test-font.ttf /tmp/crossnook-progress-books/test.epub /tmp/crossnook-progress-errors"
   ```

   Confirm the final line is exactly:

   ```text
   PROGRESS ERROR SMOKE failures=0 -> OK
   ```

4. Run process A, allow it to exit, then run process B against the same state
   directory:

   ```powershell
   C:\platform-tools\adb.exe shell "/tmp/crossnook-progress-test --save /tmp/test-font.ttf /tmp/crossnook-progress-books/test.epub /tmp/crossnook-progress-books/valid2.epub /tmp/crossnook-progress-books/foreign.epub /tmp/crossnook-progress-state"
   C:\platform-tools\adb.exe shell "/tmp/crossnook-progress-test --restore /tmp/test-font.ttf /tmp/crossnook-progress-books/test.epub /tmp/crossnook-progress-books/valid2.epub /tmp/crossnook-progress-books/foreign.epub /tmp/crossnook-progress-state"
   ```

   Confirm every assertion is `[OK]` and the output includes exactly:

   ```text
   PROGRESS relayout pages=79->254 page=26->82 progress=3210
   PROGRESS PROCESS RESTART failures=0 -> OK
   ```

5. Run the interactive application with its explicit state directory:

   ```powershell
   C:\platform-tools\adb.exe shell "/tmp/crossnook-reader-test /tmp/test-font.ttf /tmp/reader-fixtures /tmp/crossnook-app-state"
   ```

   Open `valid.epub`, navigate to a nontrivial page, press BACK, and exit with
   long POWER. Run the same command again and reopen `valid.epub`; confirm it
   restores the saved reading location rather than page 0. Repeat once using
   HOME to leave Reader and once by long-POWER exiting while Reader is open.

6. Save different positions in `valid.epub` and `valid2.epub`, restart the
   application with the same command, and confirm each book restores its own
   location. Confirm missing/corrupt restore diagnostics never produce a blank
   page, crash, or prevent normal page navigation and BACK/HOME behavior.

The automated and interactive procedures above produced the recorded hardware
validation pass.

## Explicit Non-Goals

No Wi-Fi, HTTP/TLS, KOSync, KOReader hashing compatibility, conflict
resolution, cloud sync, settings UI, custom fonts, Dark Reader, Focus Reading,
bookmarks, TOC, metadata extraction, FB2/TXT Reader integration, E-Ink partial
refresh, or rootfs/boot/kernel work is included.
