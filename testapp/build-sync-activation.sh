#!/usr/bin/env bash
# Build and host-validate explicit remote-authenticated sync activation.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  printf '%s\n' "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

cd "$REPO"
python testapp/kosync_mock_server.py --self-test

MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o
    cd /o
    CFLAGS="-std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
    CFLAGS="$CFLAGS -march=armv5te -O2 -Wall -Wextra -Werror -I/io/src -I/opt/bearssl/include"
    for file in account/sync_activation sync/kosync storage/storage_layout \
                settings/settings_store credentials/credential_store \
                identity/device_identity net/netsimple net/dnssimple \
                net/tlssimple time/timesimple platform/storage_verify; do
      arm-linux-musleabi-gcc $CFLAGS -c "/io/src/$file.c" \
        -o "$(basename "$file").o"
    done
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/sync-activation-test.c \
      -o sync-activation-test.o
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 \
      sync_activation.o kosync.o storage_layout.o settings_store.o \
      credential_store.o device_identity.o netsimple.o dnssimple.o tlssimple.o \
      timesimple.o storage_verify.o sync-activation-test.o \
      -Wl,--wrap=cn_settings_save \
      -Wl,--wrap=cn_timesimple_sync \
      -Wl,--wrap=cn_dnssimple_resolve_a \
      -Wl,--wrap=cn_kosync_authorize \
      -Wl,--wrap=cn_kosync_get_progress \
      -Wl,--wrap=cn_kosync_put_progress \
      -Wl,--wrap=cn_credential_store_load \
      -Wl,--wrap=cn_device_identity_load \
      /opt/bearssl/lib/libbearssl.a -o /io/testapp/crossnook-sync-activation-test
    file /io/testapp/crossnook-sync-activation-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-sync-activation-test | grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-sync-activation-test | grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-sync-activation-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    if readelf -l /io/testapp/crossnook-sync-activation-test | \
       grep -E "INTERP|DYNAMIC"; then
      echo "FAIL: dynamic sections present"
      exit 1
    fi
    ROOT=/tmp/crossnook-sync-activation-host
    rm -rf "$ROOT"
    mkdir -p "$ROOT"
    OUTPUT="$ROOT/output"
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
      /io/testapp/crossnook-sync-activation-test --smoke >"$OUTPUT" 2>&1
    if grep -Eiq "activation-synthetic-user|activation-synthetic-key|activation-synthetic-wrong-key|password|userkey" "$OUTPUT"; then
      echo "FAIL: activation diagnostic leaked account material"
      exit 1
    fi
    cat "$OUTPUT"
    echo "SYNC ACTIVATION HOST VALIDATION OK"
    rm -rf "$ROOT"
  '

ls -la testapp/crossnook-sync-activation-test
sha256sum testapp/crossnook-sync-activation-test
