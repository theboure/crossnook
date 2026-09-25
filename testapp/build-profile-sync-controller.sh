#!/usr/bin/env bash
# Build + validate the resolved profile/controller seam diagnostic.
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
  CFLAGS="-std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie -march=armv5te -O2 -Wall -Wextra -Werror -I/io/src -I/opt/bearssl/include -I/opt/freetype/include/freetype2"
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/sync_controller.c -o sync_controller.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/sync_push_controller.c -o sync_push_controller.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/sync_pull_controller.c -o sync_pull_controller.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/profile_sync_controller.c -o profile_sync_controller.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/profile-sync-controller-test.c -o profile_sync_controller_test.o
  for file in sync/kosync_push sync/kosync_pull sync/kosync_sync sync/kosync_policy sync/kosync settings/settings_store credentials/credential_store progress/book_identity progress/progress_store net/netsimple net/tlssimple net/dnssimple time/timesimple book/koreader_identity book/md5; do
    arm-linux-musleabi-gcc $CFLAGS -c "/io/src/$file.c" -o "$(basename "$file").o"
  done
  arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie -march=armv5te -O2 \
    -I/io/src -I/io/src/reader -I/opt/crengine/include \
    -I/opt/freetype/include/freetype2 -include stdint.h \
    -c /io/src/reader/reader.cpp -o reader.o
  arm-linux-musleabi-g++ -static -no-pie -fno-pie -march=armv5te -O2 \
    sync_controller.o sync_push_controller.o sync_pull_controller.o \
    profile_sync_controller.o profile_sync_controller_test.o \
    kosync_push.o kosync_pull.o kosync_sync.o kosync_policy.o kosync.o \
    settings_store.o credential_store.o book_identity.o progress_store.o \
    netsimple.o tlssimple.o dnssimple.o timesimple.o koreader_identity.o md5.o \
    reader.o \
    -Wl,--wrap=cn_settings_load -Wl,--wrap=cn_credential_store_load \
    -Wl,--wrap=cn_credentials_clear -Wl,--wrap=cn_kosync_client_init \
    -Wl,--wrap=cn_kosync_sync_once -Wl,--wrap=cn_kosync_push_local_once \
    -Wl,--wrap=cn_kosync_pull_remote_once \
    /opt/bearssl/lib/libbearssl.a /opt/crengine/lib/libcrengine.a \
    /opt/freetype/lib/libfreetype.a /opt/zlib/lib/libz.a \
    /opt/xxhash/lib/libxxhash.a -lm \
    -o /io/testapp/crossnook-profile-sync-controller-test
  file /io/testapp/crossnook-profile-sync-controller-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-profile-sync-controller-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-profile-sync-controller-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-profile-sync-controller-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  if readelf -l /io/testapp/crossnook-profile-sync-controller-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  ROOT=$(mktemp -d /tmp/crossnook-profile-controller-host.XXXXXX)
  trap "rm -rf \"\$ROOT\"" EXIT
  set +e
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-profile-sync-controller-test --smoke > "$ROOT/output" 2>&1
  status=$?
  set -e
  if grep -Fq -e "synthetic-profile-user" -e "synthetic-profile-userkey" \
      "$ROOT/output" /io/docs/milestone-profile-sync-controller.md; then
    echo "FAIL: synthetic credential leaked into output or documentation"
    exit 1
  fi
  if [ "$status" -ne 0 ]; then
    cat "$ROOT/output"
    echo "FAIL: profile controller smoke"
    exit 1
  fi
  cat "$ROOT/output"
  echo "PROFILE SYNC CONTROLLER HOST VALIDATION OK"
  '
sha256sum "$REPO/testapp/crossnook-profile-sync-controller-test"
