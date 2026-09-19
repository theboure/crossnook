#!/usr/bin/env bash
# Build + host-validate the Library -> Reader Integration milestone
# (crossnook-reader-test) with the pinned musl toolchain. Requires the
# toolchain image (toolchain/build-smoke.sh); host modes run under qemu-arm.
# No Nook needed.
#
# Also rebuilds + reruns the CREngine regression (build-cre.sh) so the
# spike output acts as the page-bitmap baseline that validate-reader.py
# compares the reader layer against.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> CREngine regression first (reader equivalence baseline)"
bash "$REPO/testapp/build-cre.sh"

echo "==> regenerating reader fixtures deterministically"
bash "$REPO/testapp/reader-fixtures/make-fixtures.sh"

echo "==> embedding reader stylesheets (cr3/epub/html5.css)"
if command -v python >/dev/null 2>&1; then PY=python
elif command -v python3 >/dev/null 2>&1; then PY=python3
else PY=""
fi
if [ -n "$PY" ]; then
  $PY "$REPO/src/reader/embed-css.py" "$REPO/src/reader/cre_css.h"
else
  echo "python not found; keeping committed cre_css.h"
fi

echo "==> cross-compiling crossnook-reader-test (static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o && cd /o

    CFLAGS="-static -no-pie -fno-pie -O2 -Wall -Wextra -I/io/src \
            -I/opt/freetype/include/freetype2"

    arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/nook/input.c -o input.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/nook/display.c -o display.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/graphics/canvas.c -o canvas.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/graphics/text.c -o text.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/library/library.c -o library.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/book_identity.c -o book-identity.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/progress_store.c -o progress-store.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/ui/ui.c -o ui.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/reader-test.c -o app.o

    arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie \
      -O2 -Wall -Wextra -include stdint.h \
      -I/io/src -I/io/src/reader \
      -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
      -c /io/src/reader/reader.cpp -o reader.o

    arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie \
      -O2 -Wall -Wextra \
      app.o ui.o library.o book-identity.o progress-store.o \
      input.o display.o canvas.o text.o reader.o \
      -o /io/testapp/crossnook-reader-test \
      /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
      /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm

    echo "--- file ---"
    file /io/testapp/crossnook-reader-test
    readelf -h /io/testapp/crossnook-reader-test | grep -E "Type|Machine|Flags|Entry"
    echo "--- fully static? ---"
    readelf -l /io/testapp/crossnook-reader-test | grep -E "INTERP|Dynamic" \
      && echo "WARNING: dynamic sections present" \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- structural: fb0 opened only by display.c ---"
    OWN=$(grep -rl "\"/dev/graphics/fb0\"" /io/src --include="*.c" --include="*.cpp" || true)
    echo "files referencing fb0 literal: $OWN"
    if [ "$OWN" != "/io/src/platform/nook/display.c" ]; then
      echo "FAIL: framebuffer path escaped the display module"; exit 1
    fi
    echo "OK"

    echo "--- host smoke under qemu-arm ---"
    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    $Q /io/testapp/crossnook-reader-test --smoke \
       /io/testapp/test-font.ttf /io/testapp/reader-fixtures

    echo "--- frame dump ---"
    rm -rf /io/testapp/reader-dump && mkdir -p /io/testapp/reader-dump
    $Q /io/testapp/crossnook-reader-test --dump \
       /io/testapp/test-font.ttf \
       /io/testapp/reader-fixtures /io/testapp/reader-dump

    echo "--- artifacts ---"
    ls -la /io/testapp/reader-dump
  '

echo "==> dump validation (validate-reader.py vs cre regression)"
PY="python"
$PY "$REPO/testapp/validate-reader.py" \
    "$REPO/testapp/reader-dump" "$REPO/testapp/cre-dump"

echo "==> artifacts"
ls -la "$REPO/testapp/crossnook-reader-test"
sha256sum "$REPO/testapp/crossnook-reader-test"
sha256sum "$REPO/testapp/reader-fixtures/valid.epub" \
          "$REPO/testapp/reader-fixtures/valid2.epub" \
          "$REPO/testapp/reader-fixtures/broken.epub" \
          "$REPO/testapp/reader-fixtures/book.fb2" \
          "$REPO/testapp/reader-fixtures/notes.txt"
