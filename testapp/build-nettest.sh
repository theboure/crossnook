#!/usr/bin/env bash
# Build and host-validate the minimal plain-HTTP networking client.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> cross-compiling crossnook-net-test (static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o
    cd /o

    CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
    CFLAGS="$CFLAGS -O2 -Wall -Wextra -I/io/src"
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/app/nettest.c -o nettest.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/net/netsimple.c -o netsimple.o
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra \
      nettest.o netsimple.o \
      -o /io/testapp/crossnook-net-test

    echo "--- file ---"
    file /io/testapp/crossnook-net-test
    file /io/testapp/crossnook-net-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-net-test | grep -E "Type|Machine|Flags|Entry"
    readelf -h /io/testapp/crossnook-net-test | grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-net-test | grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-net-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    readelf -l /io/testapp/crossnook-net-test | grep -E "INTERP|DYNAMIC" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- isolation ---"
    test -z "$(grep -l "curl\|libcurl\|ssl\|openssl\|wpa\|koreader" \
      /io/src/net/*.c /io/src/net/*.h /io/src/app/nettest.c || true)"
    echo "OK: networking client avoids libcurl, TLS, and wpa; stays isolated"

    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    echo "--- API smoke ---"
    $Q /io/testapp/crossnook-net-test --api-smoke

    echo "--- self-refused connection ---"
    $Q /io/testapp/crossnook-net-test --self-refused

    echo "--- self-timeout connection (600ms deadline) ---"
    $Q /io/testapp/crossnook-net-test --self-timeout

    echo "--- loopback self-test against canned 200 response ---"
    $Q /io/testapp/crossnook-net-test --self-test

    echo "--- usage ---"
    if $Q /io/testapp/crossnook-net-test; then
      echo "FAIL: usage run exited zero"; exit 1
    fi
    echo "NETTEST HOST VALIDATION OK"
  '

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-net-test"
sha256sum "$REPO/testapp/crossnook-net-test"