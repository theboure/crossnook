#!/usr/bin/env bash
# Focused real-Reader/ProgressStore, fake-controller ARM/QEMU validation.
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
  CFLAGS="-std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie -march=armv5te -O2 -Wall -Wextra -I/io/src -I/opt/bearssl/include -I/opt/freetype/include/freetype2"
  arm-linux-musleabi-gcc $CFLAGS -Werror -c /io/src/app/reader_sync.c -o reader-sync.o
  arm-linux-musleabi-gcc $CFLAGS -Werror -c /io/src/app/reader-sync-test.c -o reader-sync-test.o
  for file in ui/ui graphics/canvas graphics/text library/library progress/book_identity progress/progress_store sync/sync_controller sync/kosync_sync sync/kosync_policy sync/kosync settings/settings_store credentials/credential_store storage/storage_layout platform/storage_verify net/netsimple net/tlssimple net/dnssimple time/timesimple book/koreader_identity book/md5; do
    arm-linux-musleabi-gcc $CFLAGS -c "/io/src/$file.c" -o "$(basename "$file").o"
  done
  arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie -march=armv5te \
    -O2 -Wall -Wextra -Wno-unused-parameter -include stdint.h \
    -I/io/src -I/io/src/reader -I/opt/crengine/include \
    -I/opt/freetype/include/freetype2 \
    -c /io/src/reader/reader.cpp -o reader.o
  arm-linux-musleabi-g++ -static -no-pie -fno-pie -march=armv5te -O2 \
    reader-sync.o reader-sync-test.o ui.o canvas.o text.o library.o \
    book_identity.o progress_store.o sync_controller.o kosync_sync.o \
    kosync_policy.o kosync.o settings_store.o credential_store.o \
    storage_layout.o storage_verify.o netsimple.o tlssimple.o dnssimple.o \
    timesimple.o koreader_identity.o md5.o reader.o \
    -Wl,--wrap=cn_book_identity_from_path \
    -Wl,--wrap=cn_ui_reader_get_position \
    -Wl,--wrap=cn_progress_store_save \
    -Wl,--wrap=cn_sync_current_book \
    /opt/bearssl/lib/libbearssl.a /opt/crengine/lib/libcrengine.a \
    /opt/freetype/lib/libfreetype.a /opt/zlib/lib/libz.a \
    /opt/xxhash/lib/libxxhash.a -lm \
    -o /io/testapp/crossnook-reader-sync-test
  file /io/testapp/crossnook-reader-sync-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-reader-sync-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-reader-sync-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-reader-sync-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-reader-sync-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-reader-sync-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  ROOT=$(mktemp -d /tmp/crossnook-reader-sync-host.XXXXXX)
  trap "rm -rf \"\$ROOT\"" EXIT
  mkdir "$ROOT/state"
  set +e
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-reader-sync-test --smoke \
    /io/testapp/test-font.ttf /io/testapp/reader-fixtures/valid.epub \
    "$ROOT/state" > "$ROOT/output" 2>&1
  status=$?
  set -e
  if grep -Fq -e "x-auth-key:" -e "Authorization:" \
       -e "crossnook-synthetic-controller-key" \
       -e "dfb450efddbb5387197c84460623675b" \
       "$ROOT/output" /io/docs/milestone-reader-sync.md; then
    echo "FAIL: secret-bearing output or milestone documentation"
    exit 1
  fi
  if [ "$status" -ne 0 ]; then
    grep -E "^\\[(OK|FAIL)\\]|^READER SYNC SMOKE" "$ROOT/output" || true
    echo "FAIL: Reader sync smoke"
    exit 1
  fi
  if qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
       /io/testapp/crossnook-reader-sync-test --smoke \
       /io/testapp/test-font.ttf /io/testapp/reader-fixtures/valid.epub \
       "$ROOT/state" unexpected > "$ROOT/invalid" 2>&1; then
    echo "FAIL: extra CLI arguments accepted"
    exit 1
  fi
  if qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
       /io/testapp/crossnook-reader-sync-test --physical run upload \
       /tmp/crossnook-reader-sync-absent \
       /tmp/crossnook-reader-sync-absent/crossnook 179 16 \
       /io/testapp/test-font.ttf /io/testapp/reader-fixtures/valid.epub \
       127.0.0.2 19153 127.0.0.2 19123 /io/testapp/pki/testca.crt \
       > "$ROOT/unmounted" 2>&1; then
    echo "FAIL: unverified storage accepted"
    exit 1
  fi
  if ! grep -Fxq "READER SYNC GATE storage=unverified persistence=not-attempted" \
       "$ROOT/unmounted"; then
    echo "FAIL: unmounted gate did not fail closed"
    exit 1
  fi
  grep -E "^\\[(OK|FAIL)\\]|^READER SYNC SMOKE" "$ROOT/output"
  echo "READER SYNC HOST VALIDATION OK"
'
sha256sum "$REPO/testapp/crossnook-reader-sync-test"
