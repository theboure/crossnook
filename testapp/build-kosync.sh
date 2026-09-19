#!/usr/bin/env bash
# Build and host-validate the bounded plain-HTTP KOSync protocol client.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> validating deterministic Python mock server"
python "$REPO/testapp/kosync_mock_server.py" --self-test

python "$REPO/testapp/kosync_mock_server.py" --host 127.0.0.1 --port 18080 \
  --timestamp-start 1700000000 --quiet >/tmp/crossnook-kosync-mock.log 2>&1 &
MOCK_PID=$!
trap 'kill "$MOCK_PID" 2>/dev/null || true' EXIT
sleep 1

echo "==> cross-compiling crossnook-kosync-test (static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  --add-host=host.docker.internal:host-gateway \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o
    cd /o

    CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
    CFLAGS="$CFLAGS -O2 -Wall -Wextra -Werror -I/io/src"
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/kosync-test.c -o kosync-test.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/kosync.c -o kosync.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/netsimple.c -o netsimple.o
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra -Werror \
      kosync-test.o kosync.o netsimple.o -o /io/testapp/crossnook-kosync-test

    echo "--- file ---"
    file /io/testapp/crossnook-kosync-test
    file /io/testapp/crossnook-kosync-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-kosync-test | grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-kosync-test | grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-kosync-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    readelf -l /io/testapp/crossnook-kosync-test | grep -E "INTERP|DYNAMIC" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- isolation ---"
    test -z "$(grep -l "libcurl\|openssl\|mbedtls\|wolfssl" \
      /io/src/sync/*.c /io/src/sync/*.h /io/src/net/*.c /io/src/net/*.h || true)"
    echo "OK: KOSync protocol core remains plain HTTP without TLS libraries"

    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    echo "--- API and golden fixture smoke ---"
    $Q /io/testapp/crossnook-kosync-test --api-smoke

    echo "--- mock integration ---"
    $Q /io/testapp/crossnook-kosync-test --mock-smoke http://host.docker.internal:18080
    $Q /io/testapp/crossnook-kosync-test --roundtrip \
      http://host.docker.internal:18080 test-user dfb450efddbb5387197c84460623675b \
      e1a1e9016cfc9bca8c694187943e9c4f 519220cea448409961e6b3081a36eca3

    echo "--- network error propagation ---"
    $Q /io/testapp/crossnook-kosync-test --network-error http://127.0.0.2:1

    echo "--- usage ---"
    if $Q /io/testapp/crossnook-kosync-test; then
      echo "FAIL: usage run exited zero"; exit 1
    fi
    echo "KOSYNC HOST VALIDATION OK"
  '

kill "$MOCK_PID"
wait "$MOCK_PID" 2>/dev/null || true
trap - EXIT

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-kosync-test"
sha256sum "$REPO/testapp/crossnook-kosync-test"
