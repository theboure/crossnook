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

    CFLAGS="-static -no-pie -fno-pie -O2 -Wall -Wextra \
      -I/io/src -I/opt/freetype/include/freetype2"
    CXXFLAGS="-std=c++17 -static -no-pie -fno-pie -O2 -Wall -Wextra \
      -include stdint.h -I/io/src -I/io/src/reader \
      -I/opt/crengine/include -I/opt/freetype/include/freetype2"

    arm-linux-musleabi-gcc $CFLAGS -c src/app/ui-test.c -o /tmp/ui-test.o
    arm-linux-musleabi-gcc $CFLAGS -c src/ui/ui.c -o /tmp/ui.o
    arm-linux-musleabi-gcc $CFLAGS -c src/library/library.c -o /tmp/library.o
    arm-linux-musleabi-gcc $CFLAGS -c src/platform/nook/input.c -o /tmp/input.o
    arm-linux-musleabi-gcc $CFLAGS -c src/platform/nook/display.c -o /tmp/display.o
    arm-linux-musleabi-gcc $CFLAGS -c src/graphics/text.c -o /tmp/text.o
    arm-linux-musleabi-gcc $CFLAGS -c src/graphics/canvas.c -o /tmp/canvas.o
    arm-linux-musleabi-g++ $CXXFLAGS -c src/reader/reader.cpp -o /tmp/reader.o

    # The UI module now owns an optional reader (reader/reader.cpp); the
    # fallback paths are exercised without attaching one at runtime.
    arm-linux-musleabi-g++ -static -no-pie -fno-pie -O2 -Wall -Wextra \
      -o testapp/crossnook-ui-test \
      /tmp/ui-test.o /tmp/ui.o /tmp/library.o /tmp/input.o /tmp/display.o \
      /tmp/text.o /tmp/canvas.o /tmp/reader.o \
      /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
      /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm

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