#!/usr/bin/env bash
# Build and host-validate the portable storage layout core.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  printf '%s\n' "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> cross-compiling crossnook-storage-layout-test"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o
    cd /o

    CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
    CFLAGS="$CFLAGS -O2 -Wall -Wextra -Werror -I/io/src"
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/app/storage-layout-test.c -o storage-layout-test.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/storage/storage_layout.c -o storage-layout.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/progress/progress_store.c -o progress-store.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/progress/book_identity.c -o book-identity.o
    arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie \
      -O2 -Wall -Wextra -include stdint.h -I/io/src -I/io/src/reader \
      -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
      -c /io/src/reader/reader.cpp -o reader.o
    arm-linux-musleabi-g++ -static -no-pie -fno-pie -O2 -Wall -Wextra \
      storage-layout-test.o storage-layout.o progress-store.o \
      book-identity.o reader.o \
      /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
      /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm \
      -o /io/testapp/crossnook-storage-layout-test

    file /io/testapp/crossnook-storage-layout-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-storage-layout-test | \
      grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-storage-layout-test | \
      grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-storage-layout-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    readelf -l /io/testapp/crossnook-storage-layout-test | \
      grep -E "INTERP|DYNAMIC" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "ARM static EABI5 soft-float non-PIE -> OK"

    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    ROOT=/tmp/crossnook-storage-layout-host
    rm -rf "$ROOT"
    mkdir -p "$ROOT"
    $Q /io/testapp/crossnook-storage-layout-test --smoke "$ROOT"
    DENIED=/tmp/crossnook-storage-layout-denied
    rm -rf "$DENIED"
    mkdir -p "$DENIED"
    chmod 0500 "$DENIED"
    su -s /bin/sh nobody -c \
      "$Q /io/testapp/crossnook-storage-layout-test --expect-prepare $DENIED permission-denied"
    chmod 0700 "$DENIED"
    rm -rf "$DENIED"
    rm -rf "$ROOT"
    echo "STORAGE LAYOUT HOST VALIDATION OK"
  '

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-storage-layout-test"
sha256sum "$REPO/testapp/crossnook-storage-layout-test"
