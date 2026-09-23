#!/usr/bin/env bash
# Focused synthetic mount/device verification on the ARM target under QEMU.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG=crossnook-toolchain
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  printf '%s\n' "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi
MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
  set -euo pipefail
  export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
  mkdir -p /o
  cd /o
  CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie -march=armv5te -O2 -Wall -Wextra -Werror -I/io/src"
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/storage_verify.c -o storage-verify.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/storage-verify-test.c -o storage-verify-test.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/storage/storage_layout.c -o storage-layout.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/settings/settings_store.c -o settings-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/progress_store.c -o progress-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/book_identity.c -o book-identity.o
  arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie -march=armv5te \
    -O2 -Wall -Wextra -Wno-unused-parameter -include stdint.h \
    -I/io/src -I/io/src/reader \
    -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
    -c /io/src/reader/reader.cpp -o reader.o
  arm-linux-musleabi-g++ -static -no-pie -fno-pie -march=armv5te -O2 \
    storage-verify.o storage-verify-test.o storage-layout.o settings-store.o \
    progress-store.o book-identity.o reader.o \
    /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
    /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm \
    -o /io/testapp/crossnook-storage-verify-test
  file /io/testapp/crossnook-storage-verify-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-storage-verify-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-storage-verify-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-storage-verify-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-storage-verify-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-storage-verify-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-storage-verify-test --smoke
  echo "STORAGE VERIFY HOST VALIDATION OK"
'
sha256sum "$REPO/testapp/crossnook-storage-verify-test"
