#!/usr/bin/env bash
# Build + host-validate crossnook-text with the pinned musl toolchain and
# the FreeType static cross library from the same image.
# Requires: docker image `crossnook-toolchain` (rebuild via
#           toolchain/build-smoke.sh after the Dockerfile change).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> cross-compiling crossnook-text (static, EABI5 soft-float, FreeType 2.13.3)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io/testapp \
  "$IMG" bash -c '
    set -e
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra \
        -I/opt/freetype/include/freetype2 \
        -L/opt/freetype/lib \
        -o crossnook-text crossnook-text.c -lfreetype
    echo "--- file ---"
    file crossnook-text
    echo "--- readelf -h ---"
    readelf -h crossnook-text | grep -E "Type|Machine|Flags|Entry"
    echo "--- fully static? ---"
    readelf -l crossnook-text | grep -E "INTERP|Dynamic" && \
        echo "WARNING: dynamic sections present" || \
        echo "no INTERP/Dynamic -> fully static"
    echo "--- attributes ---"
    readelf -A crossnook-text | grep -iE "Float|VFP|Tag_CPU_arch|Tag_ABI_VFP_args" || true
    echo "--- freetype symbols linked? ---"
    arm-linux-musleabi-nm crossnook-text | grep -cE " T FT_" || true

    echo "--- SMOKE (FT init + Latin + Cyrillic glyph loads) ---"
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
        ./crossnook-text --smoke /io/testapp/test-font.ttf
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
        ./crossnook-text --smoke /io/testapp/test-font.ttf | grep -q "SMOKE OK"

    echo "--- RENDER to fb-text.bin ---"
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
        ./crossnook-text --render /io/testapp/test-font.ttf /io/testapp/fb-text.bin
    echo "--- render output size ---"
    ls -la /io/testapp/fb-text.bin
    test "$(stat -c %s /io/testapp/fb-text.bin)" = "960000" && \
        echo "size OK: 960000"
  '

echo "==> artifacts"
ls -la "$REPO/testapp/crossnook-text"
sha256sum "$REPO/testapp/crossnook-text"
sha256sum "$REPO/testapp/test-font.ttf"