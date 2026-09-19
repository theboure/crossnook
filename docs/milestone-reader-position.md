# Milestone: Reader Logical Position / Sync Foundation

**Status: HARDWARE VALIDATION PASS** (real Nook Simple Touch). The automated
logical-position smoke test and manual Library-to-Reader regression checks
passed on-device. This milestone adds no persistence, document hashing,
network access, KOSync protocol, settings UI, or visible Reader behavior.

## Scope

The Reader layer now provides a stable logical-position boundary for future
local progress and optional sync code:

```
CREngine XPointer (private to reader.cpp)
        -> cn_reader_position (public C value, opaque UTF-8 token)
        -> future LocalProgressStore
        -> future optional KOSyncClient
```

`ui.c` is unchanged. It still knows only pages and Reader actions; no
CREngine C++ type or XPointer syntax crosses `src/reader/reader.h`.

## Public Representation

```
typedef struct cn_reader_position {
    char *location;       // owned opaque UTF-8 canonical location
    int progress_10000;   // secondary 0..10000 metadata; -1 = unknown
} cn_reader_position;
```

- `location` is canonical. It is not a page number or percentage. Callers may
  serialize it but must not parse or construct it.
- `progress_10000` is secondary display/sync metadata in hundredths of one
  percent. Restore never falls back to it.
- `cn_reader_position_init` creates an empty value.
- `cn_reader_get_position` allocates/replaces the location in an initialized
  value.
- `cn_reader_position_copy` performs a deep copy.
- `cn_reader_position_clear` frees the string, resets progress to `-1`, and is
  idempotent.
- A zero-initialized, cleared, malformed, invalid-UTF-8, overflowing, or
  overlong position is rejected by `cn_reader_goto_position` before CREngine
  parsing. A syntactically valid token that cannot resolve in the open
  document is rejected safely by CREngine without moving the Reader.

## CREngine Primitive

Capture uses `LVDocView::getBookmark(true)` to locate the first precise
logical word/character on the current page. `ldomXPointer::toString()` emits
CREngine's normalized XPointer and `UnicodeToUtf8()` makes the owned public
token.

Restore converts UTF-8 internally, calls
`ldomDocument::createXPointer()`, and reserializes the result. Restore succeeds
only when the XPointer resolves and its canonical reserialization exactly
matches the supplied token. Only then does `LVDocView::goToBookmark()` move
the view; `getBookmarkPage()` derives the physical page under the current
layout. Failed restores do not alter reader state.

The percentage follows CREngine's own bookmark calculation: resolved
vertical document coordinate divided by rendered full height, clamped to
`0..10000`. It is informative and never canonical.

## Relayout Validation

`testapp/build-position.sh` builds the static ARM
`crossnook-position-test` and runs it under qemu-arm. The test:

1. opens the deterministic 79-page EPUB with config A;
2. navigates to physical page 26 and captures an owned logical token;
3. closes/reopens with config A, resolves the token, and proves the rendered
   RGB565 page checksum is identical;
4. closes, applies config B (`font_size=40`, `margin_px=24`), and reopens;
5. proves page count changes from 79 to 254 and the same unchanged canonical
   token resolves exactly at physical page 82;
6. renders with canary guards around the 960000-byte framebuffer;
7. rejects empty, malformed, invalid-UTF-8, overflowing, overlong,
   noncanonical-but-resolvable, and syntactically valid but unresolved values
   without moving or changing the render;
8. opens a valid but structurally different generated EPUB and proves the
   saved token cannot resolve there, while the foreign document remains
   renderable;
9. deep-copies/frees positions and closes the Reader cleanly.

Exact canonical XPointer resolution is the deterministic document-location
equivalence check. Physical page changes are expected and explicitly tested.

Cross-document detection is deliberately limited to what XPointer resolution
can prove. A different document with an identical compatible DOM could accept
the same token; associating positions with a book identity belongs to a future
progress store. This milestone intentionally does not add document hashing.

## Host Validation

Run:

```
bash testapp/build-position.sh
```

Expected final line:

```
POSITION SMOKE failures=0 -> OK
```

The full regression gate also includes:

```
bash testapp/build-test.sh
bash testapp/build-text.sh
bash testapp/build-ui.sh
bash testapp/build-library.sh
bash testapp/build-cre.sh
bash testapp/build-reader.sh
```

No existing golden expectation is changed by this milestone.

## Hardware Validation

The exact automated smoke test passed on a real Nook Simple Touch with:

```
POSITION relayout pages=79->254 page=26->82 progress=3210
POSITION SMOKE failures=0 -> OK
```

The device run confirmed:

- logical position capture works;
- close/reopen restores the logical position;
- same-layout restore returns to the same physical page and renders the same
  page;
- `cn_reader_position` owns an independent position string;
- normalized progress metadata remains valid;
- layout-changing configuration changes pagination from 79 to 254 pages and
  maps page 26 to page 82 while the canonical logical token remains unchanged;
- logical position is independent of physical page number;
- empty/default, malformed, unresolved, invalid-UTF-8, overflowing, and
  overlong positions fail safely;
- a resolvable but noncanonical XPointer is rejected by canonical comparison;
- failed restores leave the rendered state unchanged;
- a position from a structurally different EPUB fails safely;
- the Reader lifecycle closes cleanly and capture after close is rejected;
- all manual Library-to-Reader regression checks pass on-device.

## Real-Nook Validation Procedure

The hardware pass above was established with this procedure.

1. Build and generate the deterministic fixtures on the host:

   ```
   bash testapp/build-position.sh
   bash testapp/build-reader.sh
   ```

2. Create a temporary device directory and push the logical-position test,
   font, primary EPUB, and structurally foreign EPUB:

   ```
   adb shell mkdir -p /tmp/crossnook-position
   adb push testapp/crossnook-position-test /tmp/crossnook-position/
   adb push testapp/test-font.ttf /tmp/crossnook-position/
   adb push testapp/cre-fixtures/test.epub /tmp/crossnook-position/
   adb push testapp/position-fixtures/foreign.epub /tmp/crossnook-position/
   adb shell chmod 755 /tmp/crossnook-position/crossnook-position-test
   ```

3. Run the exact automated position sequence on the Nook:

   ```
   adb shell /tmp/crossnook-position/crossnook-position-test --smoke \
     /tmp/crossnook-position/test-font.ttf \
     /tmp/crossnook-position/test.epub \
     /tmp/crossnook-position/foreign.epub
   ```

4. Confirm every assertion is `[OK]`, the relayout line reports changed page
   count and page number, and the final line is exactly:

   ```
   POSITION SMOKE failures=0 -> OK
   ```

5. Recheck the unchanged visible Library-to-Reader flow with the rebuilt
   integration binary:

   ```
   adb shell mkdir -p /tmp/reader-fixtures /tmp/crossnook-state
   adb push testapp/crossnook-reader-test /tmp/
   adb push testapp/test-font.ttf /tmp/
   adb push testapp/reader-fixtures/. /tmp/reader-fixtures/
   adb shell chmod 755 /tmp/crossnook-reader-test
   adb shell /tmp/crossnook-reader-test \
     /tmp/test-font.ttf /tmp/reader-fixtures /tmp/crossnook-state
   ```

6. On screen, confirm HOME -> Library, valid EPUB page rendering, NEXT/PREV,
   BACK restoring Library selection/viewport, HOME returning HOME, broken
   EPUB fallback, and long-POWER exit. Confirm no crash or framebuffer
   corruption.

Both the automated position test and visible Reader regression passed before
this milestone was marked hardware-valid.

## Explicit Non-Goals

No filesystem/database persistence, Wi-Fi, HTTP, TLS, KOReader sync protocol,
device IDs, conflict resolution, document hashing, settings, custom fonts,
Dark Reader, Focus Reading, bookmarks, TOC, or progress UI is implemented.
