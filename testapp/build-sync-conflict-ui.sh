#!/usr/bin/env bash
# Verified-card Reader conflict UI; host fake controllers, real Reader/ProgressStore.
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
  arm-linux-musleabi-gcc $CFLAGS -Werror -c /io/src/app/reader_conflict.c -o reader_conflict.o
  arm-linux-musleabi-gcc $CFLAGS -Werror -c /io/src/app/sync-conflict-ui-test.c -o sync-conflict-ui-test.o
  arm-linux-musleabi-gcc $CFLAGS -Werror -c /io/src/ui/ui.c -o ui.o
  if ! arm-linux-musleabi-nm -u reader_conflict.o | grep -q " cn_sync_push_local_current_book$" ||
     ! arm-linux-musleabi-nm -u reader_conflict.o | grep -q " cn_sync_pull_remote_current_book$" ||
     ! arm-linux-musleabi-nm -u reader_conflict.o | grep -q " cn_reader_sync_apply_persisted$"; then
    echo "FAIL: application helper must compose published explicit operations"
    exit 1
  fi
  if arm-linux-musleabi-nm -u reader_conflict.o ui.o | \
       grep -E "cn_kosync_(get_progress|put_progress|serialize_progress|sync_once)|cn_sync_current_book"; then
    echo "FAIL: UI/helper references normal sync or raw KOSync operations"
    exit 1
  fi
  for file in app/reader_sync graphics/canvas graphics/text library/library progress/book_identity progress/progress_store sync/sync_controller sync/sync_pull_controller sync/sync_push_controller sync/kosync_pull sync/kosync_push sync/kosync_sync sync/kosync_policy sync/kosync settings/settings_store credentials/credential_store storage/storage_layout platform/storage_verify platform/nook/input platform/nook/display net/netsimple net/tlssimple net/dnssimple time/timesimple book/koreader_identity book/md5; do
    arm-linux-musleabi-gcc $CFLAGS -c "/io/src/$file.c" -o "$(basename "$file").o"
  done
  arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie -march=armv5te \
    -O2 -Wall -Wextra -Wno-unused-parameter -include stdint.h \
    -I/io/src -I/io/src/reader -I/opt/crengine/include \
    -I/opt/freetype/include/freetype2 \
    -c /io/src/reader/reader.cpp -o reader.o
  arm-linux-musleabi-g++ -static -no-pie -fno-pie -march=armv5te -O2 \
    reader_conflict.o sync-conflict-ui-test.o reader_sync.o ui.o canvas.o \
    text.o library.o book_identity.o progress_store.o sync_controller.o \
    sync_pull_controller.o sync_push_controller.o kosync_pull.o kosync_push.o \
    kosync_sync.o kosync_policy.o kosync.o settings_store.o \
    credential_store.o storage_layout.o storage_verify.o input.o display.o \
    netsimple.o tlssimple.o dnssimple.o timesimple.o koreader_identity.o \
    md5.o reader.o \
    -Wl,--wrap=cn_sync_current_book \
    -Wl,--wrap=cn_sync_push_local_current_book \
    -Wl,--wrap=cn_sync_pull_remote_current_book \
    -Wl,--wrap=cn_progress_store_save \
    -Wl,--wrap=cn_ui_reader_goto_position \
    /opt/bearssl/lib/libbearssl.a /opt/crengine/lib/libcrengine.a \
    /opt/freetype/lib/libfreetype.a /opt/zlib/lib/libz.a \
    /opt/xxhash/lib/libxxhash.a -lm \
    -o /io/testapp/crossnook-sync-conflict-ui-test
  file /io/testapp/crossnook-sync-conflict-ui-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-sync-conflict-ui-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-sync-conflict-ui-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-sync-conflict-ui-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-sync-conflict-ui-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-sync-conflict-ui-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  ROOT=$(mktemp -d /tmp/crossnook-sync-conflict-host.XXXXXX)
  trap "rm -rf \"\$ROOT\"" EXIT
  mkdir "$ROOT/state"
  set +e
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-sync-conflict-ui-test --smoke \
    /io/testapp/test-font.ttf /io/testapp/reader-fixtures/valid.epub \
    "$ROOT/state" > "$ROOT/output" 2>&1
  status=$?
  set -e
  if grep -Fq -e "dfb450efddbb5387197c84460623675b" \
     -e "x-auth-key:" -e "Authorization:" "$ROOT/output" \
     /io/docs/milestone-sync-conflict-resolution.md; then
    echo "FAIL: credential-bearing diagnostic output or docs"
    exit 1
  fi
  if [ "$status" -ne 0 ]; then
    grep -E "^\\[(OK|FAIL)\\]|^SYNC CONFLICT UI SMOKE" "$ROOT/output" || true
    echo "FAIL: conflict UI smoke"
    exit 1
  fi
  if qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
       /io/testapp/crossnook-sync-conflict-ui-test --smoke \
       /io/testapp/test-font.ttf /io/testapp/reader-fixtures/valid.epub \
       "$ROOT/state" unexpected > "$ROOT/invalid" 2>&1; then
    echo "FAIL: extra diagnostic arguments accepted"
    exit 1
  fi
  if qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
       /io/testapp/crossnook-sync-conflict-ui-test --physical run conflict \
       /tmp/crossnook-sync-conflict-absent \
       /tmp/crossnook-sync-conflict-absent/crossnook 179 16 \
       /io/testapp/test-font.ttf /io/testapp/reader-fixtures/valid.epub \
       127.0.0.2 19153 127.0.0.2 19123 /io/testapp/pki/testca.crt \
       > "$ROOT/unmounted" 2>&1; then
    echo "FAIL: unverified storage accepted"
    exit 1
  fi
  if ! grep -Fxq \
       "SYNC CONFLICT GATE storage=unverified persistence=not-attempted network=not-attempted" \
       "$ROOT/unmounted"; then
    echo "FAIL: unmounted gate did not fail closed"
    exit 1
  fi
  grep -E "^\\[(OK|FAIL)\\]|^SYNC CONFLICT UI SMOKE" "$ROOT/output"
  echo "SYNC CONFLICT UI HOST VALIDATION OK"
'
sha256sum "$REPO/testapp/crossnook-sync-conflict-ui-test"
