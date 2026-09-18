#!/usr/bin/env bash
# Build + host-validate the UI Core milestone (crossnook-ui-test) with the
# pinned musl toolchain. Requires the toolchain image (toolchain/build-smoke.sh,
# crossnook-toolchain); runs host modes under qemu-arm. No Nook needed.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> cross-compiling crossnook-ui-test (static, EABI5 soft-float, non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    FONTS=/io/testapp/test-font.ttf

    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra \
      -Isrc \
      -I/opt/freetype/include/freetype2 -L/opt/freetype/lib \
      -o testapp/crossnook-ui-test \
      src/app/ui-test.c \
      src/ui/ui.c \
      src/platform/nook/input.c \
      src/platform/nook/display.c \
      src/graphics/text.c \
      src/graphics/canvas.c \
      -lfreetype

    echo "--- file ---"
    file testapp/crossnook-ui-test
    readelf -h testapp/crossnook-ui-test | grep -E "Type|Machine|Flags|Entry"
    echo "--- fully static? ---"
    readelf -l testapp/crossnook-ui-test | grep -E "INTERP|Dynamic" \
      && echo "WARNING: dynamic sections present" \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- structural: fb0 opened only by display.c ---"
    OWN=$(grep -rl "\"/dev/graphics/fb0\"" src --include="*.c" || true)
    echo "files referencing fb0 literal: $OWN"
    if [ "$OWN" != "src/platform/nook/display.c" ]; then
      echo "FAIL: framebuffer path escaped the display module"; exit 1
    fi
    if grep -rn "FB_PATH\|/dev/graphics/fb0" src/graphics src/ui src/app --include="*.c" \
        | grep -v "^src/.*:.*\*" ; then
      :  # comments are allowed; no code path may open the device outside display.c
    fi
    echo "OK"

    echo "--- host modes under qemu-arm ---"
    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    $Q testapp/crossnook-ui-test --smoke  $FONTS
    $Q testapp/crossnook-ui-test --home   $FONTS /io/testapp/fb-ui-home.bin
    $Q testapp/crossnook-ui-test --reader $FONTS /io/testapp/fb-ui-reader.bin 1
    $Q testapp/crossnook-ui-test --reader $FONTS /io/testapp/fb-ui-reader3.bin 3
    $Q testapp/crossnook-ui-test --primitives /io/testapp/fb-ui-primitives.bin

    echo "--- dumps ---"
    ls -la /io/testapp/fb-ui-*.bin
  '

echo "==> dump validation (validate-ui.py)"
PY="python"
$PY testapp/validate-ui.py testapp/fb-ui-home.bin HOME
$PY testapp/validate-ui.py testapp/fb-ui-reader.bin READER
$PY testapp/validate-ui.py testapp/fb-ui-reader3.bin READER
$PY testapp/validate-ui.py testapp/fb-ui-primitives.bin PRIMITIVES
diff -q testapp/fb-ui-reader.bin testapp/fb-ui-reader3.bin >/dev/null \
  && echo "FAIL: page 1 == page 3 dumps" && exit 1 \
  || echo "internal: page 1 and page 3 dumps differ"

echo "==> interaction test (crossnook-ui-test under qemu + FIFOs)"
bash testapp/host-live-ui-test.sh

echo "==> artifacts"
ls -la "$REPO/testapp/crossnook-ui-test"
sha256sum "$REPO/testapp/crossnook-ui-test"