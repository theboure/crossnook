#!/usr/bin/env bash
# Build + validate the local-only persisted sync profile diagnostic.
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
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/persisted_sync_profile.c -o persisted-sync-profile.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/storage/storage_layout.c -o storage-layout.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/settings/settings_store.c -o settings-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/credentials/credential_store.c -o credential-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/storage_verify.c -o storage-verify.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/persisted-sync-profile-test.c -o persisted-sync-profile-test.o
  if arm-linux-musleabi-nm -u persisted-sync-profile.o | grep -E "(socket|connect|send|recv|cn_dns|cn_tls|cn_time|cn_sync_current|cn_sync_pull|cn_sync_push|cn_settings_save|cn_credential_store_save|cn_device_identity_load_or_create|cn_storage_layout_prepare)"; then
    echo "FAIL: persisted profile references forbidden operation"
    exit 1
  fi
  arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 \
    device-identity.o persisted-sync-profile.o storage-layout.o settings-store.o \
    credential-store.o storage-verify.o persisted-sync-profile-test.o \
    -o /io/testapp/crossnook-persisted-sync-profile-test
  file /io/testapp/crossnook-persisted-sync-profile-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-persisted-sync-profile-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-persisted-sync-profile-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-persisted-sync-profile-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-persisted-sync-profile-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-persisted-sync-profile-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-persisted-sync-profile-test --smoke
  echo "PERSISTED SYNC PROFILE HOST VALIDATION OK"
  sha256sum /io/testapp/crossnook-persisted-sync-profile-test
  '
