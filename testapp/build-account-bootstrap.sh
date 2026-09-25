#!/usr/bin/env bash
# Build + validate the local-only Sync Account Bootstrap diagnostic.
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
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/account/kosync_userkey.c -o kosync-userkey.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/account/account_bootstrap.c -o account-bootstrap.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/account-bootstrap-test.c -o account-bootstrap-test.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/book/md5.c -o md5.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/storage/storage_layout.c -o storage-layout.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/settings/settings_store.c -o settings-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/credentials/credential_store.c -o credential-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/storage_verify.c -o storage-verify.o
  for object in kosync-userkey.o account-bootstrap.o account-bootstrap-test.o md5.o; do
    if arm-linux-musleabi-nm -u "$object" | grep -E "(socket|connect|send|recv|cn_kosync_client|cn_kosync_sync|cn_sync_)"; then
      echo "FAIL: account bootstrap object references network or KOSync"
      exit 1
    fi
  done
  arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 \
    -Wl,--wrap=cn_settings_save -Wl,--wrap=cn_credential_store_save \
    kosync-userkey.o account-bootstrap.o account-bootstrap-test.o md5.o \
    storage-layout.o settings-store.o credential-store.o storage-verify.o \
    -o /io/testapp/crossnook-account-bootstrap-test
  file /io/testapp/crossnook-account-bootstrap-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-account-bootstrap-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-account-bootstrap-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-account-bootstrap-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-account-bootstrap-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-account-bootstrap-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  if ! qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
      /io/testapp/crossnook-account-bootstrap-test --smoke \
      > /tmp/account-bootstrap-output; then
    cat /tmp/account-bootstrap-output
    exit 1
  fi
  if grep -F -e "test-password" -e "dfb450efddbb5387197c84460623675b" \
      /tmp/account-bootstrap-output; then
    echo "FAIL: diagnostic output exposed credential material"
    exit 1
  fi
  cat /tmp/account-bootstrap-output
  echo "ACCOUNT BOOTSTRAP HOST VALIDATION OK"
 '
sha256sum "$REPO/testapp/crossnook-account-bootstrap-test"
