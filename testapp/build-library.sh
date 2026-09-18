#!/usr/bin/env bash
# Build + host-validate the Library Core milestone (crossnook-library-test)
# with the pinned musl toolchain. Requires the toolchain image
# (toolchain/build-smoke.sh, crossnook-toolchain); runs host modes under
# qemu-arm. No Nook needed.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> cross-compiling crossnook-library-test (static, EABI5 soft-float, non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    FONTS=/io/testapp/test-font.ttf
    FIX=/io/testapp/library-fixtures
    EMPTY=$FIX/empty

    # Regenerate the fixtures deterministically (some names, e.g. the
    # '~'-terminated temp decoy, are deliberately gitignored on the host).
    bash /io/testapp/library-fixtures/make-fixtures.sh

    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra \
      -Isrc \
      -I/opt/freetype/include/freetype2 -L/opt/freetype/lib \
      -o testapp/crossnook-library-test \
      src/app/ui-library-test.c \
      src/ui/ui.c \
      src/library/library.c \
      src/platform/nook/input.c \
      src/platform/nook/display.c \
      src/graphics/text.c \
      src/graphics/canvas.c \
      -lfreetype

    echo "--- file ---"
    file testapp/crossnook-library-test
    readelf -h testapp/crossnook-library-test | grep -E "Type|Machine|Flags|Entry"
    echo "--- fully static? ---"
    readelf -l testapp/crossnook-library-test | grep -E "INTERP|Dynamic" \
      && echo "WARNING: dynamic sections present" \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- structural: fb0 opened only by display.c ---"
    OWN=$(grep -rl "\"/dev/graphics/fb0\"" src --include="*.c" || true)
    echo "files referencing fb0 literal: $OWN"
    if [ "$OWN" != "src/platform/nook/display.c" ]; then
      echo "FAIL: framebuffer path escaped the display module"; exit 1
    fi
    echo "OK"

    echo "--- host modes under qemu-arm ---"
    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    $Q testapp/crossnook-library-test --smoke $FONTS $FIX
    $Q testapp/crossnook-library-test --dump-library  $FONTS $FIX  /io/testapp/fb-library.bin
    $Q testapp/crossnook-library-test --dump-scrolled $FONTS $FIX  /io/testapp/fb-library-scrolled.bin
    $Q testapp/crossnook-library-test --dump-empty    $FONTS $EMPTY /io/testapp/fb-library-empty.bin
    $Q testapp/crossnook-library-test --dump-book     $FONTS $FIX  /io/testapp/fb-library-book.bin

    echo "--- dumps ---"
    ls -la /io/testapp/fb-library-*.bin
  '

echo "==> dump validation (validate-library.py)"
PY="python"
$PY testapp/validate-library.py testapp/fb-library.bin LIBRARY
$PY testapp/validate-library.py testapp/fb-library-scrolled.bin SCROLLED
$PY testapp/validate-library.py testapp/fb-library-empty.bin EMPTY
$PY testapp/validate-library.py testapp/fb-library-book.bin BOOK
diff -q testapp/fb-library.bin testapp/fb-library-scrolled.bin >/dev/null \
  && echo "FAIL: library == scrolled dumps" && exit 1 \
  || echo "internal: scrolled dump differs from the first viewport"

echo "==> interaction test (crossnook-library-test under qemu + FIFOs)"
bash testapp/host-live-library-test.sh

echo "==> artifacts"
ls -la "$REPO/testapp/crossnook-library-test"
sha256sum "$REPO/testapp/crossnook-library-test"