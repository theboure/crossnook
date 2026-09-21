#!/usr/bin/env bash
# Build and host-validate bounded DNS plus DNS-routed verified HTTPS.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG="crossnook-toolchain"
BASE=19253
HTTPS_PORT=19443
PIDS=""
SNI_LOG="work/crossnook-dns-sni.log"
HOST_LOG="work/crossnook-dns-host.log"
DNS_TRACE_CFLAGS=""
DNS_BUILD_ONLY="${CN_DNS_BUILD_ONLY:-0}"

if [ "${CN_DNS_TRACE:-0}" = "1" ]; then
  DNS_TRACE_CFLAGS="-DCN_DNS_DIAGNOSTIC_TRACE=1"
fi

cd "$REPO"

cleanup() {
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

set -- $(MSYS_NO_PATHCONV=1 docker run --rm \
  --add-host=host.docker.internal:host-gateway \
  "$IMG" getent ahostsv4 host.docker.internal)
HOST_IP=$1
mkdir -p work
rm -f "$SNI_LOG" "$HOST_LOG"

if [ "$DNS_BUILD_ONLY" != "1" ]; then
  python testapp/dns_mock_server.py --host 0.0.0.0 \
    --port "$BASE" --answer "$HOST_IP" --suite --verbose \
    >work/crossnook-dns-mock.log 2>&1 &
  DNS_PID=$!
  PIDS="$PIDS $DNS_PID"
  python testapp/https_mock_server.py --host 0.0.0.0 --port "$HTTPS_PORT" \
    --cert testapp/pki/server.crt \
    --key testapp/pki/server.key \
    --sni-log "$SNI_LOG" --host-log "$HOST_LOG" --quiet \
    >work/crossnook-dns-https.log 2>&1 &
  HTTPS_PID=$!
  PIDS="$PIDS $HTTPS_PID"
  sleep 1
  kill -0 "$DNS_PID"
  kill -0 "$HTTPS_PID"
fi

echo "==> cross-compiling DNS-enabled crossnook-net-test"
MSYS_NO_PATHCONV=1 docker run --rm \
  --add-host=host.docker.internal:host-gateway \
  -e DNS_TRACE_CFLAGS="$DNS_TRACE_CFLAGS" \
  -e DNS_BUILD_ONLY="$DNS_BUILD_ONLY" \
  -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o
    cd /o

    CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
    CFLAGS="$CFLAGS -O2 -Wall -Wextra -Werror -I/io/src -I/opt/bearssl/include"
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/nettest.c -o nettest.o
    arm-linux-musleabi-gcc $CFLAGS $DNS_TRACE_CFLAGS \
      -c /io/src/net/dnssimple.c -o dnssimple.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/netsimple.c -o netsimple.o
    arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/tlssimple.c -o tlssimple.o
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra -Werror \
      nettest.o dnssimple.o netsimple.o tlssimple.o \
      /opt/bearssl/lib/libbearssl.a -o /io/testapp/crossnook-net-test

    file /io/testapp/crossnook-net-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-net-test | grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-net-test | grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-net-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    readelf -l /io/testapp/crossnook-net-test | grep -E "INTERP|DYNAMIC" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "ARM static EABI5 soft-float non-PIE -> OK"

    if [ "$DNS_BUILD_ONLY" = "1" ]; then
      echo "DNS build only -> OK"
      exit 0
    fi

    set -- $(getent ahostsv4 host.docker.internal)
    HOST_IP=$1
    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    BIN=/io/testapp/crossnook-net-test
    BASE=19253

    expect_fail() {
      name=$1
      port=$2
      expected=$3
      set +e
      output=$($Q $BIN --dns-query "$HOST_IP" "$port" 200 \
        secure.test.local 2>&1)
      status=$?
      set -e
      printf "%s: %s\n" "$name" "$output"
      test "$status" -ne 0
      printf "%s\n" "$output" | grep -q "NETTEST DNS FAIL $expected"
    }

    echo "--- focused deterministic DNS cases ---"
    output=$($Q $BIN --dns-query "$HOST_IP" "$BASE" 300 secure.test.local)
    printf "valid-single: %s\n" "$output"
    printf "%s\n" "$output" | \
      grep -q "NETTEST DNS OK.*count=1.*server=0.*addr0=$HOST_IP"

    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 1)) 300 secure.test.local)
    printf "multiple: %s\n" "$output"
    printf "%s\n" "$output" | grep -q \
      "NETTEST DNS OK.*count=3.*addr0=10.20.30.1.*addr1=10.20.30.2.*addr2=10.20.30.3"

    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 2)) 300 secure.test.local)
    printf "duplicate: %s\n" "$output"
    printf "%s\n" "$output" | grep -q "NETTEST DNS OK.*count=1"

    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 3)) 300 secure.test.local)
    printf "bounded-many: %s\n" "$output"
    printf "%s\n" "$output" | grep -q \
      "NETTEST DNS OK.*count=4.*addr0=10.20.30.1.*addr1=10.20.30.2.*addr2=10.20.30.3.*addr3=10.20.30.4"

    expect_fail nxdomain $((BASE + 4)) nxdomain
    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 5)) 300 \
      secure.test.local "$HOST_IP")
    printf "servfail-fallback: %s\n" "$output"
    printf "%s\n" "$output" | grep -q "NETTEST DNS OK.*server=1"
    expect_fail timeout $((BASE + 6)) timeout
    expect_fail wrong-id $((BASE + 7)) timeout
    expect_fail wrong-source $((BASE + 8)) timeout
    expect_fail truncated-packet $((BASE + 9)) malformed-response
    expect_fail oversized $((BASE + 10)) packet-size
    expect_fail tc $((BASE + 11)) truncated-response
    expect_fail mismatched-question $((BASE + 12)) malformed-response
    expect_fail bad-pointer $((BASE + 13)) malformed-response
    expect_fail pointer-loop $((BASE + 14)) malformed-response
    expect_fail zero-address $((BASE + 15)) no-address
    expect_fail unrelated-owner $((BASE + 16)) no-address
    expect_fail cname-only $((BASE + 17)) unsupported-cname
    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 18)) 300 secure.test.local)
    printf "unusual-unrelated-owner: %s\n" "$output"
    printf "%s\n" "$output" | grep -q "NETTEST DNS OK.*count=1"
    expect_fail reserved-z-bit $((BASE + 19)) malformed-response
    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 20)) 800 \
      secure.test.local)
    printf "drop-first-two-retransmits: %s\n" "$output"
    printf "%s\n" "$output" | grep -q "NETTEST DNS OK.*count=1.*server=0"

    set +e
    bounded_started=$(date +%s%N)
    bounded=$(timeout 1.7s $Q $BIN --dns-query "$HOST_IP" \
      $((BASE + 21)) 1000 secure.test.local 2>&1)
    bounded_status=$?
    bounded_finished=$(date +%s%N)
    set -e
    bounded_ms=$(((bounded_finished - bounded_started) / 1000000))
    printf "drop-all-bounded (%s ms): %s\n" "$bounded_ms" "$bounded"
    test "$bounded_status" -eq 1
    test "$bounded_ms" -ge 850
    test "$bounded_ms" -le 1500
    printf "%s\n" "$bounded" | grep -q "NETTEST DNS FAIL timeout"

    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 22)) 800 \
      secure.test.local)
    printf "delayed-initial-response: %s\n" "$output"
    printf "%s\n" "$output" | grep -q "NETTEST DNS OK.*count=1.*server=0"

    output=$($Q $BIN --dns-query "$HOST_IP" $((BASE + 23)) 800 \
      secure.test.local)
    printf "delayed-first-retransmit-response: %s\n" "$output"
    printf "%s\n" "$output" | grep -q "NETTEST DNS OK.*count=1.*server=0"

    set +e
    invalid=$($Q $BIN --dns-query "$HOST_IP" "$BASE" 200 bad..name 2>&1)
    invalid_status=$?
    set -e
    printf "invalid-hostname: %s\n" "$invalid"
    test "$invalid_status" -ne 0
    printf "%s\n" "$invalid" | grep -q "NETTEST DNS FAIL invalid-hostname"

    echo "--- DNS-routed verified HTTPS ---"
    output=$($Q $BIN --https-get-dns "$HOST_IP" "$BASE" 300 19443 \
      secure.test.local /io/testapp/pki/testca.crt /https/get \
      1789948800 1000)
    printf "%s\n" "$output"
    printf "%s\n" "$output" | \
      grep -q "NETTEST DNS ROUTE.*address=$HOST_IP.*server=0"
    printf "%s\n" "$output" | \
      grep -q "NETTEST HTTPS DNS GET 200 23 184 OK"

    set +e
    wrong=$($Q $BIN --https-get-dns "$HOST_IP" "$BASE" 300 19443 \
      wrong.test /io/testapp/pki/testca.crt /https/get \
      1789948800 1000 2>&1)
    wrong_status=$?
    set -e
    printf "wrong-original-host: %s\n" "$wrong"
    test "$wrong_status" -ne 0
    printf "%s\n" "$wrong" | \
      grep -q "NETTEST DNS ROUTE.*address=$HOST_IP.*server=0"
    printf "%s\n" "$wrong" | \
      grep -q "NETTEST HTTPS DNS GET FAIL tls-hostname-mismatch"

    echo "--- focused existing regressions ---"
    $Q $BIN --api-smoke
    $Q $BIN --tls-time /io/testapp/pki
    echo "DNS HOST VALIDATION OK"
  '

if [ "$DNS_BUILD_ONLY" = "1" ]; then
  echo "==> artifact"
  ls -la "$REPO/testapp/crossnook-net-test"
  sha256sum "$REPO/testapp/crossnook-net-test"
  exit 0
fi

python -c '
import pathlib
import re

text = pathlib.Path("work/crossnook-dns-mock.log").read_text(encoding="utf-8")

def receive_times(mode):
    return [
        int(match.group(1))
        for line in text.splitlines()
        if f"mode={mode}" in line
        for match in [re.search(r"mono_ns=(\d+)", line)]
        if match
    ]

for mode, expected_ms, tolerance_ms in (
    ("drop-first-two-then-valid", (200, 400), 75),
    ("drop-all-fourth-valid", (250, 500), 100),
    ("delayed-first-after-two-retransmits", (200, 400), 75),
    ("delayed-second-after-retransmit", (200, 400), 75),
):
    times = receive_times(mode)
    assert len(times) == 3, (mode, len(times))
    offsets_ms = tuple((value - times[0]) / 1_000_000 for value in times[1:])
    assert all(
        expected - tolerance_ms <= actual <= expected + tolerance_ms
        for actual, expected in zip(offsets_ms, expected_ms)
    ), (mode, offsets_ms)
    print(
        f"DNS MOCK {mode} count=3 "
        f"offsets_ms={offsets_ms[0]:.1f},{offsets_ms[1]:.1f} -> OK"
    )

assert "IDENTITY-ERROR" not in text
print("DNS all three query bytes + transaction IDs identical -> OK")
'

grep -qx 'secure.test.local' "$SNI_LOG"
grep -qx 'wrong.test' "$SNI_LOG"
echo "DNS HTTPS SNI secure.test.local + wrong.test -> OK"
grep -qx 'secure.test.local:19443' "$HOST_LOG"
echo "DNS HTTPS Host secure.test.local:19443 -> OK"

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-net-test"
sha256sum "$REPO/testapp/crossnook-net-test"
