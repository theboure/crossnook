#!/usr/bin/env bash
# Fake-driven controller host validation; no live network or real credentials.
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
  CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie -march=armv5te -O2 -Wall -Wextra -Werror -I/io/src -I/opt/bearssl/include"
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/sync_controller.c -o sync-controller.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/sync-controller-test.c -o sync-controller-test.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/settings/settings_store.c -o settings-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/credentials/credential_store.c -o credential-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/storage/storage_layout.c -o storage-layout.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/storage_verify.c -o storage-verify.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/kosync_sync.c -o kosync-sync.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/kosync_policy.c -o kosync-policy.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/kosync.c -o kosync.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/netsimple.c -o netsimple.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/tlssimple.c -o tlssimple.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/dnssimple.c -o dnssimple.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/time/timesimple.c -o timesimple.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/book/koreader_identity.c -o koreader-identity.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/book/md5.c -o md5.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/book_identity.c -o book-identity.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/progress_store.c -o progress-store.o
  arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie -march=armv5te \
    -O2 -Wall -Wextra -Wno-unused-parameter -include stdint.h \
    -I/io/src -I/io/src/reader -I/opt/crengine/include \
    -I/opt/freetype/include/freetype2 \
    -c /io/src/reader/reader.cpp -o reader.o
  arm-linux-musleabi-g++ -static -no-pie -fno-pie -march=armv5te -O2 \
    sync-controller.o sync-controller-test.o settings-store.o credential-store.o \
    storage-layout.o storage-verify.o kosync-sync.o kosync-policy.o kosync.o \
    netsimple.o tlssimple.o dnssimple.o timesimple.o koreader-identity.o \
    md5.o book-identity.o progress-store.o reader.o \
    -Wl,--wrap=cn_settings_load -Wl,--wrap=cn_credential_store_load \
    -Wl,--wrap=cn_credentials_clear -Wl,--wrap=cn_kosync_sync_once \
    -Wl,--wrap=cn_kosync_policy_classify \
    /opt/bearssl/lib/libbearssl.a /opt/crengine/lib/libcrengine.a \
    /opt/freetype/lib/libfreetype.a /opt/zlib/lib/libz.a \
    /opt/xxhash/lib/libxxhash.a -lm \
    -o /io/testapp/crossnook-sync-controller-test
  file /io/testapp/crossnook-sync-controller-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-sync-controller-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-sync-controller-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-sync-controller-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-sync-controller-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-sync-controller-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  ROOT=$(mktemp -d /tmp/crossnook-controller-host.XXXXXX)
  trap "rm -rf \"\$ROOT\"" EXIT
  set +e
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-sync-controller-test --smoke > "$ROOT/output" 2>&1
  status=$?
  set -e
  if grep -Fq -e "crossnook-synthetic-controller-account" \
     -e "crossnook-synthetic-controller-key" \
     -e "dfb450efddbb5387197c84460623675b" "$ROOT/output" \
     /io/docs/milestone-sync-controller.md; then
    echo "FAIL: synthetic credential leaked into output or documentation"
    exit 1
  fi
  if [ "$status" -ne 0 ]; then
    cat "$ROOT/output"
    echo "FAIL: controller smoke"
    exit 1
  fi
  if qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
       /io/testapp/crossnook-sync-controller-test --smoke unexpected \
       > "$ROOT/invalid" 2>&1; then
    echo "FAIL: diagnostic accepted credential arguments"
    exit 1
  fi
  if grep -Fq -e "crossnook-synthetic-controller-account" \
     -e "crossnook-synthetic-controller-key" \
     -e "dfb450efddbb5387197c84460623675b" "$ROOT/invalid"; then
    echo "FAIL: invalid CLI leaked a synthetic credential"
    exit 1
  fi
  cat "$ROOT/output"
  echo "SYNC CONTROLLER HOST VALIDATION OK"
'
sha256sum "$REPO/testapp/crossnook-sync-controller-test"
