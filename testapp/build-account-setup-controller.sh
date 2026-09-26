#!/usr/bin/env bash
# Build + validate the account setup controller diagnostic.
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
  CFLAGS="-std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
  CFLAGS="$CFLAGS -march=armv5te -O2 -Wall -Wextra -Werror -I/io/src -I/opt/bearssl/include"
  for file in account/account_setup_controller account/account_bootstrap \
              account/kosync_userkey account/sync_activation sync/kosync \
              sync/persisted_sync_profile storage/storage_layout \
              settings/settings_store credentials/credential_store \
              identity/device_identity net/netsimple net/dnssimple \
              net/tlssimple time/timesimple platform/storage_verify book/md5; do
    arm-linux-musleabi-gcc $CFLAGS -c "/io/src/$file.c" \
      -o "$(basename "$file").o"
  done
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/account-setup-controller-test.c \
    -o account-setup-controller-test.o
  arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 \
    account_setup_controller.o account_bootstrap.o kosync_userkey.o sync_activation.o \
    kosync.o persisted_sync_profile.o storage_layout.o settings_store.o \
    credential_store.o device_identity.o netsimple.o dnssimple.o tlssimple.o \
    timesimple.o storage_verify.o md5.o account-setup-controller-test.o \
    -Wl,--wrap=cn_settings_save -Wl,--wrap=cn_credential_store_save \
    -Wl,--wrap=cn_credential_store_load -Wl,--wrap=cn_device_identity_load \
    -Wl,--wrap=cn_device_identity_load_or_create \
    -Wl,--wrap=cn_kosync_userkey_from_password -Wl,--wrap=cn_timesimple_sync \
    -Wl,--wrap=cn_dnssimple_resolve_a -Wl,--wrap=cn_kosync_authorize \
    -Wl,--wrap=cn_kosync_get_progress -Wl,--wrap=cn_kosync_put_progress \
    -Wl,--wrap=cn_persisted_sync_profile_load \
    /opt/bearssl/lib/libbearssl.a -o /io/testapp/crossnook-account-setup-controller-test
  file /io/testapp/crossnook-account-setup-controller-test | \
    grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-account-setup-controller-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-account-setup-controller-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-account-setup-controller-test | \
    grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  if readelf -l /io/testapp/crossnook-account-setup-controller-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  ROOT=$(mktemp -d /tmp/crossnook-account-setup-host.XXXXXX)
  trap "rm -rf \"\$ROOT\"" EXIT
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-account-setup-controller-test --smoke >"$ROOT/output" 2>&1
  if grep -Eiq "setup-synthetic|password|userkey|0123456789abcdef" "$ROOT/output"; then
    echo "FAIL: account material leaked into diagnostic output"
    exit 1
  fi
  cat "$ROOT/output"
  echo "ACCOUNT SETUP CONTROLLER HOST VALIDATION OK"
  '
sha256sum "$REPO/testapp/crossnook-account-setup-controller-test"
