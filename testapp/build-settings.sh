#!/usr/bin/env bash
# Build and host-validate the Persistent Settings Core.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  printf '%s\n' "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> cross-compiling crossnook-settings-test"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o
    cd /o

    CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
    CFLAGS="$CFLAGS -march=armv5te"
    CFLAGS="$CFLAGS -O2 -Wall -Wextra -Werror -I/io/src"
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/app/settings-store-test.c -o settings-store-test.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/settings/settings_store.c -o settings-store.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/storage/storage_layout.c -o storage-layout.o

    if arm-linux-musleabi-nm -u settings-store.o | \
       grep -E " (malloc|calloc|realloc|free)$"; then
      echo "FAIL: Settings Store references heap allocation"
      exit 1
    fi
    if grep -Eiq \
       "username|userkey|password|token|wifi|psk|device_id|dns|sntp|cookie|private|encryption|credential|secret|auth|trigger|reader|typography|presentation|font|margin|spacing|books|library|ca_bundle|ca_path|entropy|storage_root|cache|logging|boot" \
       /io/src/settings/settings_store.h; then
      echo "FAIL: excluded field appears in public Settings schema"
      exit 1
    fi

    arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 \
      settings-store-test.o settings-store.o storage-layout.o \
      -Wl,--wrap=write -Wl,--wrap=fsync -Wl,--wrap=close \
      -Wl,--wrap=rename \
      -o /io/testapp/crossnook-settings-test

    file /io/testapp/crossnook-settings-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-settings-test | \
      grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-settings-test | \
      grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-settings-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    readelf -A /io/testapp/crossnook-settings-test | \
      grep -q "Tag_CPU_arch: v5TE"
    readelf -l /io/testapp/crossnook-settings-test | \
      grep -E "INTERP|DYNAMIC" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "ARM static EABI5 soft-float non-PIE -> OK"

    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    ROOT=/tmp/crossnook-settings-host
    rm -rf "$ROOT"
    mkdir -p "$ROOT"
    $Q /io/testapp/crossnook-settings-test --smoke "$ROOT"

    DENIED=/tmp/crossnook-settings-denied
    rm -rf "$DENIED"
    mkdir -p "$DENIED"
    chmod 0500 "$DENIED"
    su -s /bin/sh nobody -c \
      "$Q /io/testapp/crossnook-settings-test --expect-save $DENIED permission-denied"
    chmod 0700 "$DENIED"
    rm -rf "$DENIED" "$ROOT"
    echo "SETTINGS STORE HOST VALIDATION OK"
  '

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-settings-test"
sha256sum "$REPO/testapp/crossnook-settings-test"
