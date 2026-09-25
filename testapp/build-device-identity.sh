#!/usr/bin/env bash
# Build + validate the standalone persistent device identity diagnostic.
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
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/identity/device_identity.c -o device-identity.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/storage/storage_layout.c -o storage-layout.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/storage_verify.c -o storage-verify.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/device-identity-test.c -o device-identity-test.o
  arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 \
    device-identity.o storage-layout.o storage-verify.o device-identity-test.o \
    -o /io/testapp/crossnook-device-identity-test
  file /io/testapp/crossnook-device-identity-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-device-identity-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-device-identity-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-device-identity-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-device-identity-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-device-identity-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  if arm-linux-musleabi-nm -u device-identity.o | grep -E "(cn_platform_storage_verify|socket|connect|recv|send)"; then
    echo "FAIL: identity module references verification or networking"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-device-identity-test --smoke
  echo "DEVICE IDENTITY HOST VALIDATION OK"
 '
sha256sum "$REPO/testapp/crossnook-device-identity-test"
