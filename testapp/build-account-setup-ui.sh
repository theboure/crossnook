#!/usr/bin/env bash
# Build the Account Setup UI host gate and static Nook application executable.
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
  mkdir -p /o && cd /o
  CFLAGS="-std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie -march=armv5te -O2 -Wall -Wextra -I/io/src -I/opt/bearssl/include -I/opt/freetype/include/freetype2"
  SOURCES="account/account_setup_controller account/account_bootstrap account/kosync_userkey account/sync_activation sync/kosync sync/persisted_sync_profile storage/storage_layout settings/settings_store credentials/credential_store identity/device_identity net/netsimple net/dnssimple net/tlssimple time/timesimple platform/storage_verify book/md5 ui/ui ui/account_text_input graphics/canvas graphics/text library/library platform/nook/input platform/nook/display app/account_setup_ui"
  for file in $SOURCES; do
    arm-linux-musleabi-gcc $CFLAGS -Werror -c "/io/src/$file.c" -o "$(basename "$file").o"
  done
  arm-linux-musleabi-gcc $CFLAGS -Werror -c /io/src/app/account-setup-ui-test.c -o account-setup-ui-test.o
  arm-linux-musleabi-gcc $CFLAGS -Werror -c /io/src/app/account-setup-ui.c -o account-setup-ui.o
  arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie -march=armv5te -O2 -Wall -Wextra -Wno-unused-parameter -include stdint.h -I/io/src -I/io/src/reader -I/opt/crengine/include -I/opt/freetype/include/freetype2 -c /io/src/reader/reader.cpp -o reader.o
  COMMON="account_setup_controller.o account_bootstrap.o kosync_userkey.o sync_activation.o kosync.o persisted_sync_profile.o storage_layout.o settings_store.o credential_store.o device_identity.o netsimple.o dnssimple.o tlssimple.o timesimple.o storage_verify.o md5.o ui.o account_text_input.o canvas.o text.o library.o input.o display.o account_setup_ui.o reader.o /opt/bearssl/lib/libbearssl.a /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm"
  arm-linux-musleabi-g++ -static -no-pie -fno-pie -march=armv5te -O2 account-setup-ui-test.o $COMMON -o /io/testapp/crossnook-account-setup-ui-test
  arm-linux-musleabi-g++ -static -no-pie -fno-pie -march=armv5te -O2 account-setup-ui.o $COMMON -o /io/testapp/crossnook-account-setup-ui
  for exe in /io/testapp/crossnook-account-setup-ui-test /io/testapp/crossnook-account-setup-ui; do
    file "$exe" | grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h "$exe" | grep -q "Type:.*EXEC"
    readelf -h "$exe" | grep -q "Machine:.*ARM"
    readelf -h "$exe" | grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    if readelf -l "$exe" | grep -E "INTERP|DYNAMIC"; then exit 1; fi
  done
  ROOT=$(mktemp -d /tmp/crossnook-account-ui.XXXXXX)
  trap "rm -rf \"\$ROOT\"" EXIT
  if ! qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
      /io/testapp/crossnook-account-setup-ui-test /io/testapp/test-font.ttf \
      >"$ROOT/output" 2>&1; then
    cat "$ROOT/output"
    exit 1
  fi
  if grep -F -e "setup.synthetic.invalid" -e "Synthetic Reader" \
      -e "qwertyuiop" "$ROOT/output"; then
    echo "FAIL: account UI diagnostic leaked form data"
    exit 1
  fi
  cat "$ROOT/output"
  echo "ACCOUNT SETUP UI HOST VALIDATION OK"
'
sha256sum "$REPO/testapp/crossnook-account-setup-ui"
