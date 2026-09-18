# crossnook-library-test — library list composition diagnostic

`crossnook-library-test` composes the reusable module tree with the
filesystem book scanner: it boots the same HOME screen as
`crossnook-ui-test`, adds a `[ Open library ]` button, and navigates a
book list with keys and touch. It supersedes nothing — `crossnook-ui-test`
remains the UI Core composition reference.

## Modes

- Device: `crossnook-library-test <font.ttf> <books-dir>`
- `--smoke <font> <dir>` — host assertions (scanner rules, sort, empty
  dir, UI transitions, ellipsis clipping).
- `--dump-library <font> <dir> <out>` — HOME -> enter library, dump the
  first viewport (sel=0 top=0).
- `--dump-scrolled <font> <dir> <out>` — enter library + 14 PAGE_NEXT
  presses (sel=14 top=14).
- `--dump-empty <font> <emptydir> <out>` — enter an empty library.
- `--dump-book <font> <dir> <out>` — enter library, select+activate row 0.

Host modes run under qemu-arm with no device access.

## Device UI

The device loop matches `crossnook-ui-test`: renders HOME, flushes the
framebuffer, polls all input devices, and logs:

- `UI initial state=HOME page=1 inputs=N books=M` (on boot)
- `UI touch=x,y` (committed TOUCH_UP coordinates)
- `UI state=<STATE> page=N` on every redraw
- `UI library sel=%d top=%d of=%d rows=%d` in LIBRARY/SELECTED_BOOK
- `UI selected title="..." format=...` on SELECTED_BOOK
- `UI long-power exit` before terminating after a >= 2 s POWER hold

Semantic `INPUT MENU/BACK/HOME/POWER_DOWN/POWER_UP` traces go to stderr.

The touch marker is a diagnostic from UI Core bring-up: it is drawn only
on HOME and the Reader Test screen. Library and Selected Book screens
intentionally do not render it, so the normal library UI stays clean.

## Host validation

`bash testapp/build-library.sh` cross-compiles (static, non-PIE, soft-float,
EABI5), checks the fb0 boundary, regenerates the fixtures, runs all five
host modes under qemu-arm, validates the four dumps with
`testapp/validate-library.py` (size 960000, margins, per-kind bands, PNG
previews), and scripts the two live sessions in
`testapp/host-live-library-test.sh` (fixtures + empty dir).

### Deploy to the Nook

Build artifacts appear as `testapp/crossnook-library-test` and
`testapp/test-font.ttf`. Pushing books to `/tmp/books`:

```
adb shell mkdir -p /tmp/books
adb push testapp/crossnook-library-test /tmp/
adb push testapp/test-font.ttf /tmp/
adb shell chmod 755 /tmp/crossnook-library-test
adb push books/ /tmp/books/        # your .epub/.fb2/.txt files
adb shell /tmp/crossnook-library-test /tmp/test-font.ttf /tmp/books
```

After a >= 2 s POWER hold the binary exits.

## Regression references

`crossnook-ui-test` (UI Core), `crossnook-text` (FreeType renderer) and
`crossnook-test` (input/bringup) must keep passing and their dumps must
stay byte-identical.