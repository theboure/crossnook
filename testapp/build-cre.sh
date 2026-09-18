#!/usr/bin/env bash
# Build + host-validate the CREngine EPUB Rendering Spike
# (crossnook-cre-test) with the pinned musl toolchain. Requires the
# crossnook-toolchain image (toolchain/build-smoke.sh); host modes run under
# qemu-arm. No Nook needed.
#
# Renders END-TO-END through CREngine (crengine libcrengine.a opens the EPUB,
# lays out 600x800 pages, draws into an RGB565 buffer) and validates the
# pages: dimensions, non-empty ink, page-to-page differences, back-to-PREV
# determinism, and canary-guarded no-OOB writes.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> embedding CREngine stylesheets (cr3/epub/html5.css)"
if command -v python >/dev/null 2>&1; then PY=python
elif command -v python3 >/dev/null 2>&1; then PY=python3
else PY=""
fi
if [ -n "$PY" ]; then
  $PY "$REPO/testapp/cre-fixtures/embed-css.py" "$REPO/testapp/cre-fixtures/cre_css.h"
else
  echo "python not found; keeping committed cre_css.h"
fi

echo "==> cross-compiling crossnook-cre-test (C++ CREngine, static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o && cd /o

    arm-linux-musleabi-gcc -static -fno-pie -O2 -Wall -Wextra -Isrc \
      -c /io/src/platform/nook/input.c -o input.o
    arm-linux-musleabi-gcc -static -fno-pie -O2 -Wall -Wextra -Isrc \
      -c /io/src/platform/nook/display.c -o display.o
    arm-linux-musleabi-gcc -static -fno-pie -O2 -Wall -Wextra -Isrc \
      -c /io/src/graphics/canvas.c -o canvas.o

    arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie \
      -O2 -Wall -Wextra -include stdint.h \
      -I/io/testapp -I/io/testapp/cre-fixtures -I/io/src \
      -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
      /io/testapp/crossnook-cre-test.cpp input.o display.o canvas.o \
      -o /io/testapp/crossnook-cre-test \
      /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
      /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm

    echo "--- file ---"
    file /io/testapp/crossnook-cre-test
    readelf -h /io/testapp/crossnook-cre-test | grep -E "Type|Machine|Flags|Entry"
    echo "--- fully static? ---"
    readelf -l /io/testapp/crossnook-cre-test | grep -E "INTERP|Dynamic" \
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
    $Q /io/testapp/crossnook-cre-test --smoke \
       --font /io/testapp/test-font.ttf \
       /io/testapp/cre-fixtures/test.epub

    echo "--- page dump ---"
    rm -rf /io/testapp/cre-dump && mkdir -p /io/testapp/cre-dump
    $Q /io/testapp/crossnook-cre-test --dump /io/testapp/cre-dump \
       --font /io/testapp/test-font.ttf \
       /io/testapp/cre-fixtures/test.epub

    echo "--- artifacts ---"
    ls -la /io/testapp/cre-dump
  '

echo "==> dump validation (validate-cre.py)"
PY=python
$PY "$REPO/testapp/validate-cre.py" "$REPO/testapp/cre-dump"

echo "==> fixture checksum (deterministic regeneration)"
bash "$REPO/testapp/cre-fixtures/make-fixtures.sh"

echo "==> artifacts"
ls -la "$REPO/testapp/crossnook-cre-test"
sha256sum "$REPO/testapp/crossnook-cre-test" "$REPO/testapp/cre-fixtures/test.epub"