#!/usr/bin/env bash
# Build and host-validate the standalone trusted-time bootstrap.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG="crossnook-toolchain"
BASE=19123
MOCK_PID=""

cleanup() {
  if [ "$MOCK_PID" ]; then
    kill "$MOCK_PID" 2>/dev/null || true
    wait "$MOCK_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

python "$REPO/testapp/sntp_mock_server.py" --host 0.0.0.0 \
  --port "$BASE" --timestamp 1790000000 --suite \
  >/tmp/crossnook-sntp-mock.log 2>&1 &
MOCK_PID=$!
sleep 1

echo "==> cross-compiling crossnook-time-test (static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  --add-host=host.docker.internal:host-gateway \
  -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o
    cd /o

    CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
    CFLAGS="$CFLAGS -O2 -Wall -Wextra -Werror -I/io/src"
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/app/timetest.c -o timetest.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/time/timesimple.c -o timesimple.o
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra -Werror \
      timetest.o timesimple.o -o /io/testapp/crossnook-time-test

    file /io/testapp/crossnook-time-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-time-test | grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-time-test | grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-time-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    readelf -l /io/testapp/crossnook-time-test | grep -E "INTERP|DYNAMIC" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "ARM static EABI5 soft-float non-PIE -> OK"

    set -- $(getent ahostsv4 host.docker.internal)
    HOST_IP=$1
    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    BIN=/io/testapp/crossnook-time-test
    BASE=19123

    expect_fail() {
      name=$1
      port=$2
      expected=$3
      set +e
      output=$($Q $BIN --query "$port" 200 "$HOST_IP" 2>&1)
      status=$?
      set -e
      printf "%s: %s\n" "$name" "$output"
      test "$status" -ne 0
      printf "%s\n" "$output" | grep -q "TIMETEST QUERY FAIL $expected"
    }

    echo "--- focused deterministic SNTP cases ---"
    output=$($Q $BIN --query "$BASE" 300 "$HOST_IP")
    printf "valid: %s\n" "$output"
    printf "%s\n" "$output" | grep -q "TIMETEST QUERY OK unix=1790000000"
    expect_fail originate-mismatch $((BASE + 1)) originate-mismatch
    expect_fail truncated $((BASE + 2)) packet-size
    expect_fail oversized $((BASE + 3)) packet-size
    expect_fail unsynchronized $((BASE + 4)) unsynchronized
    expect_fail kiss-of-death $((BASE + 5)) kiss-of-death
    expect_fail invalid-stratum $((BASE + 6)) stratum
    expect_fail zero-receive $((BASE + 7)) zero-receive
    expect_fail zero-transmit $((BASE + 8)) zero-transmit
    expect_fail old-time $((BASE + 9)) time-range
    expect_fail future-time $((BASE + 10)) time-range
    expect_fail timeout $((BASE + 11)) timeout
    # Docker Desktop may filter this alternate-source datagram before recvfrom;
    # either way it must not be accepted as the configured peer.
    expect_fail wrong-peer-rejected $((BASE + 12)) timeout
    expect_fail bad-version $((BASE + 13)) version
    expect_fail bad-mode $((BASE + 14)) mode

    output=$($Q $BIN --query "$BASE" 120 127.0.0.2 "$HOST_IP")
    printf "sequential-fallback: %s\n" "$output"
    printf "%s\n" "$output" | \
      grep -q "TIMETEST QUERY OK unix=1790000000.*server=1"

    echo "TIME HOST VALIDATION OK"
  '

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-time-test"
sha256sum "$REPO/testapp/crossnook-time-test"
