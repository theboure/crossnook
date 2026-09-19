# Milestone: KOReader-Compatible Document Identity

**Status: HOST VALIDATION PASS; HARDWARE VALIDATION PASS.** CrossNook can
produce KOReader/KOSync-compatible Binary and Filename document identities.
This milestone adds no networking and does not change Local Progress identity
or persisted state.

## Upstream Pin

Compatibility was researched against KOReader commit:

```text
8da811b1bd4f33c2ce239885fe96e24771aba308
```

The revision was the upstream `HEAD` resolved on 2026-09-19. All source review
and oracle work used immutable commit URLs, not a floating branch. Relevant
pins are:

| Component | Revision |
| --- | --- |
| `koreader/koreader` | `8da811b1bd4f33c2ce239885fe96e24771aba308` |
| `koreader/koreader-base` gitlink | `9a8729713ab73f539b607af23ede6aa89d04cfed` |
| KOReader-pinned LuaJIT | `1edc3e52b67eaf6ce5f809be8e17d6862594b8bc` |

Inspected upstream files:

- `frontend/util.lua`: `util.partialMD5()` and `util.splitFilePathName()`;
- `frontend/apps/reader/readerui.lua`: creation and persistence of
  `partial_md5_checksum`;
- `plugins/kosync.koplugin/main.lua`: Binary/Filename selection and document
  digest access; and
- pinned `koreader-base/ffi/sha2.lua`: streaming MD5 input and lowercase hex
  output.

Immutable sources:

- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/frontend/util.lua>
- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/frontend/apps/reader/readerui.lua>
- <https://github.com/koreader/koreader/blob/8da811b1bd4f33c2ce239885fe96e24771aba308/plugins/kosync.koplugin/main.lua>
- <https://github.com/koreader/koreader-base/blob/9a8729713ab73f539b607af23ede6aa89d04cfed/ffi/sha2.lua>

## Exact Binary Behavior

`util.partialMD5()` opens the file with `io.open(path, "rb")`, creates one
streaming MD5 state, and performs the Lua loop `for i = -1, 10`. Each
iteration seeks absolutely to `bit.lshift(1024, 2*i)` and reads at most 1,024
bytes. LuaJIT BitOp masks shift counts to five bits, so the `i = -1` shift
produces offset zero. The resulting ordered offsets are:

| Sample | Absolute offset | Maximum bytes |
| ---: | ---: | ---: |
| 0 | 0 | 1,024 |
| 1 | 1,024 | 1,024 |
| 2 | 4,096 | 1,024 |
| 3 | 16,384 | 1,024 |
| 4 | 65,536 | 1,024 |
| 5 | 262,144 | 1,024 |
| 6 | 1,048,576 | 1,024 |
| 7 | 4,194,304 | 1,024 |
| 8 | 16,777,216 | 1,024 |
| 9 | 67,108,864 | 1,024 |
| 10 | 268,435,456 | 1,024 |
| 11 | 1,073,741,824 | 1,024 |

Each nonempty read is fed directly to the same MD5 updater. The MD5 input is
the concatenation of the sampled bytes only. It contains no offsets, lengths,
separators, total file size, zero padding, filename, ZIP metadata, or EPUB
metadata. At most 12,288 file bytes are read and hashed.

For an ordinary seekable file:

- an empty file returns the normal MD5 of empty input;
- files through 2,048 bytes are effectively hashed in full;
- a short read contributes only the bytes actually returned;
- no bytes are added for an empty read;
- the first sample offset at or beyond EOF ends sampling;
- a file ending exactly at a sample offset does not include that sample;
- extending the file by one byte across a sample offset adds that one byte;
- bytes in gaps between sample windows do not affect the identity; and
- output is exactly 32 lowercase hexadecimal MD5 characters.

KOReader ignores the return value from `file:seek()`. Its Lua I/O layer treats
a zero-byte read as `nil`, so normal EOF breaks the loop. CrossNook returns an
explicit I/O error for failed seek/read/close operations instead of returning a
digest of whatever prefix happened to be read. Successful regular-file
identity behavior is byte-for-byte compatible.

`ReaderUI` saves this value as `partial_md5_checksum`. KOSync Binary mode
returns that saved value rather than recalculating it during each sync.

## KOSync Selection

KOReader defines Binary as method `0`, Filename as method `1`, and defaults to
Binary. KOSync selects Filename only when the setting equals `1`; all other
values follow the Binary branch.

Filename mode takes the exact document path string, selects the bytes after
the final forward slash, and applies normal MD5. It therefore:

- excludes the directory path;
- includes the basename and extension;
- preserves case, encoding, whitespace, and punctuation;
- performs no Unicode or case normalization;
- does not treat a backslash as a separator; and
- hashes an empty basename as empty input.

CrossNook implements Filename mode because it shares the small MD5 primitive,
but Binary remains the required/default compatibility target.

## Architecture

The local and future-sync identities remain intentionally separate:

```text
Local Progress identity                 KOReader sync identity
src/progress/book_identity.*            src/book/koreader_identity.*
path-v1-...                              partial MD5 / filename MD5
        |                                         |
        v                                         v
  ProgressStore only                   future SyncProvider only
```

The future composition boundary is:

```text
ReaderPosition
      |
      v
ProgressStore     BookIdentity
      |                |
      +---- SyncProvider ----+
                             |
                             v
                       KOSyncClient
```

Public API:

```c
cn_book_identity_koreader_binary(path, identity)
cn_book_identity_koreader_filename(path, identity)
cn_koreader_document_id_text(identity)
```

`src/book/md5.*` is a private, allocation-free RFC 1321 implementation. No
suitable MD5 API is exported by the currently linked CREngine, FreeType, zlib,
or xxHash archives. A small private implementation avoids OpenSSL or another
runtime dependency. MD5 is used only as a compatibility digest, not for
security.

The Binary implementation keeps one 1 KiB sample buffer and seeks directly to
the upstream offsets. It never reads the full document into memory. All sample
offsets fit the target's signed 32-bit `long`, including old Linux 2.6.29.

No KOReader identity code is present in `ui.c`, `reader.cpp`,
`progress_store.*`, or `library.c`. Existing Local Progress files are not
renamed, migrated, or re-keyed.

## Independent Oracle

Expected values were generated independently from the C implementation:

1. check out KOReader at `8da811b1bd4f33c2ce239885fe96e24771aba308`;
2. check out `koreader-base` at its pinned gitlink
   `9a8729713ab73f539b607af23ede6aa89d04cfed`;
3. build KOReader's pinned LuaJIT revision
   `1edc3e52b67eaf6ce5f809be8e17d6862594b8bc`;
4. load the exact pinned `ffi/sha2.lua` MD5 implementation; and
5. execute the isolated `util.partialMD5()` loop from pinned `util.lua`
   against the deterministic EPUB and generated sparse boundary fixtures.

The full fixed output is committed in `testapp/bookid-vectors.txt`. Ordinary
regression runs require no network, KOReader checkout, Lua, or LuaJIT.

Required EPUB vectors:

| Fixture | KOReader Binary document ID |
| --- | --- |
| `testapp/cre-fixtures/test.epub` | `e1a1e9016cfc9bca8c694187943e9c4f` |
| `testapp/reader-fixtures/valid2.epub` | `e1a1e9016cfc9bca8c694187943e9c4f` |
| `testapp/position-fixtures/foreign.epub` | `519220cea448409961e6b3081a36eca3` |

Selected boundary vectors:

| Fixture behavior | KOReader Binary document ID |
| --- | --- |
| empty file | `d41d8cd98f00b204e9800998ecf8427e` |
| EOF exactly at 4,096 | `599af7d5ee812c83b1f5f7afa943ac30` |
| EOF at 4,097 | `8a0b8ca0172f86a3be35f775035ab721` |
| all twelve full sample regions | `7e5018e58f5a6dd40ba48159c4c0993f` |
| change in unsampled gap | unchanged: `422fcf48372040f93b6e005f39d5a482` |
| change inside sampled region | `9c94da2f139487a2d41f83c792af1b89` |

The fixture generator creates sparse files around every sample-start EOF
boundary through 1 GiB inside the ephemeral build container. These catch
whole-file hashing, wrong offsets, wrong read sizes, skipped first samples,
zero padding, bad EOF handling, 32-bit offset mistakes, and hashing hex text
instead of concatenated sample bytes.

## Host Validation

Run:

```sh
bash testapp/build-bookid.sh
```

The suite verifies:

- all fixed Binary vectors from the pinned Lua oracle;
- small files and every sampling/EOF boundary;
- all twelve sample offsets, including the 1 GiB offset;
- sampled and unsampled byte modifications;
- same bytes under different paths and filenames;
- deterministic repeated calculation;
- exact Filename basename/extension/case/slash behavior;
- missing, directory/unreadable, empty, and oversized inputs;
- standard MD5 padding boundaries at 55 and 56 bytes;
- static ARM EABI5 non-PIE output and QEMU execution;
- structural isolation from UI, Reader, ProgressStore, and Library; and
- unchanged Local Progress vector
  `books/error.epub -> path-v1-9e285c67105cacd8`.

Observed final result:

```text
BOOKID HOST VALIDATION OK
```

## Hardware Validation

Focused validation passed on a physical Nook Simple Touch. The static ARM
`crossnook-bookid-test --api-smoke` completed with no failures. Binary mode
produced these exact on-device values:

| Fixture | Real-Nook Binary document ID |
| --- | --- |
| `test.epub` | `e1a1e9016cfc9bca8c694187943e9c4f` |
| `valid2.epub` | `e1a1e9016cfc9bca8c694187943e9c4f` |
| `foreign.epub` | `519220cea448409961e6b3081a36eca3` |

The real ARM outputs match the pinned independent KOReader/LuaJIT oracle
exactly.

KOReader Binary identity is a sampled partial-MD5 fingerprint, not a complete
file hash. Files differing only outside sampled regions may intentionally
produce the same document ID. `test.epub` and `valid2.epub` are the validated
same-ID example in this fixture set; the current deterministic files are
byte-identical, which guarantees matching sampled bytes.

## Focused Real-Nook Validation

No networking is required. Run these commands from PowerShell at the
repository root:

```powershell
bash testapp/build-bookid.sh
C:\platform-tools\adb.exe devices
C:\platform-tools\adb.exe shell "rm -rf /tmp/crossnook-bookid; mkdir -p /tmp/crossnook-bookid"
C:\platform-tools\adb.exe push testapp/crossnook-bookid-test /tmp/
C:\platform-tools\adb.exe push testapp/cre-fixtures/test.epub /tmp/crossnook-bookid/
C:\platform-tools\adb.exe push testapp/reader-fixtures/valid2.epub /tmp/crossnook-bookid/
C:\platform-tools\adb.exe push testapp/position-fixtures/foreign.epub /tmp/crossnook-bookid/
C:\platform-tools\adb.exe shell "chmod 755 /tmp/crossnook-bookid-test"
C:\platform-tools\adb.exe shell "/tmp/crossnook-bookid-test --api-smoke"
C:\platform-tools\adb.exe shell "/tmp/crossnook-bookid-test --binary /tmp/crossnook-bookid/test.epub"
C:\platform-tools\adb.exe shell "/tmp/crossnook-bookid-test --binary /tmp/crossnook-bookid/valid2.epub"
C:\platform-tools\adb.exe shell "/tmp/crossnook-bookid-test --binary /tmp/crossnook-bookid/foreign.epub"
```

Confirm the API smoke ends with:

```text
BOOKID API SMOKE failures=0 -> OK
```

Confirm the three Binary outputs are exactly:

```text
BOOKID binary e1a1e9016cfc9bca8c694187943e9c4f
BOOKID binary e1a1e9016cfc9bca8c694187943e9c4f
BOOKID binary 519220cea448409961e6b3081a36eca3
```

The focused ARM procedure above produced the recorded hardware validation pass.

## Limitations And Non-Goals

Binary identity means matching sampled bytes, not semantic book equivalence.
Repacking or editing an EPUB can change its identity when sampled bytes change,
even if visible content appears equivalent. Conversely, modifications entirely
outside sampled regions do not change this partial digest and can intentionally
share a KOSync document ID.

This milestone includes no Wi-Fi, HTTP, TLS, KOSync API calls, credentials,
server URLs, device IDs, push/pull, conflict resolution, automatic sync,
progress merging, Local Progress migration, settings UI, custom fonts, Dark
Reader, Focus Reading, bookmarks, TOC, partial E-Ink refresh, or system-image
work.
