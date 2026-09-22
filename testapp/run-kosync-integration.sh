#!/usr/bin/env bash
# Runs inside the CrossNook toolchain container from build-kosync.sh.
set -euo pipefail
export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
mkdir -p /o
cd /o

BEARSSL=/opt/bearssl
BEARSSL_LIB=/opt/bearssl/lib/libbearssl.a
if test -f /io/work/bearssl-check/libbearssl.a; then
  BEARSSL=/io/work/bearssl-check
  BEARSSL_LIB=/io/work/bearssl-check/libbearssl.a
fi
CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie"
CFLAGS="$CFLAGS -O2 -Wall -Wextra -Werror -I/io/src -I$BEARSSL/include"
arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/kosync-test.c -o kosync-test.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/kosync.c -o kosync.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/kosync_sync.c -o kosync-sync.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/sync/kosync_policy.c -o kosync-policy.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/netsimple.c -o netsimple.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/tlssimple.c -o tlssimple.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/net/dnssimple.c -o dnssimple.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/time/timesimple.c -o timesimple.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/book/koreader_identity.c -o koreader-identity.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/book/md5.c -o md5.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/book_identity.c -o book-identity.o
arm-linux-musleabi-gcc $CFLAGS -c /io/src/progress/progress_store.c -o progress-store.o
arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie \
  -O2 -Wall -Wextra -include stdint.h -I/io/src -I/io/src/reader \
  -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
  -c /io/src/reader/reader.cpp -o reader.o
arm-linux-musleabi-g++ -static -no-pie -fno-pie -O2 -Wall -Wextra \
  kosync-test.o kosync.o kosync-sync.o kosync-policy.o netsimple.o tlssimple.o \
  dnssimple.o timesimple.o koreader-identity.o md5.o book-identity.o \
  progress-store.o reader.o "$BEARSSL_LIB" \
  /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
  /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm \
  -o /io/testapp/crossnook-kosync-test

file /io/testapp/crossnook-kosync-test | \
  grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
readelf -h /io/testapp/crossnook-kosync-test | grep -q "Type:.*EXEC"
readelf -h /io/testapp/crossnook-kosync-test | grep -q "Machine:.*ARM"
readelf -h /io/testapp/crossnook-kosync-test | \
  grep -q "Flags:.*Version5 EABI.*soft-float ABI"
readelf -l /io/testapp/crossnook-kosync-test | grep -E "INTERP|DYNAMIC" \
  && { echo "FAIL: dynamic sections present"; exit 1; } \
  || echo "ARM static EABI5 soft-float non-PIE -> OK"

Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
BIN=/io/testapp/crossnook-kosync-test
set -- $(getent ahostsv4 host.docker.internal)
HOST_IP=$1
URL=https://secure.test.local:19443
WRONG_URL=https://wrong.test:19443
CA=/io/testapp/pki/testca.crt
EPUB=/io/testapp/cre-fixtures/test.epub
DNS_PORT=19353
EPOCH=1790000000
KEY=dfb450efddbb5387197c84460623675b
ROOT=/tmp/crossnook-kosync-integration
rm -rf "$ROOT"
mkdir -p "$ROOT"

make_credentials() {
  user=$1
  key=${2:-$KEY}
  printf "%s\n%s\n" "$user" "$key" >"$ROOT/$user.credentials"
  chmod 600 "$ROOT/$user.credentials"
}

sync_once() {
  user=$1
  state=$2
  url=${3:-$URL}
  dns_port=${4:-$DNS_PORT}
  time_policy=${5:-caller-established}
  epoch=${6:-$EPOCH}
  recv_ms=${7:-1000}
  sntp_server=${8:-127.0.0.2}
  $Q $BIN --sync-once "$state" "$EPUB" "$HOST_IP" "$dns_port" 300 \
    "$time_policy" "$sntp_server" 19123 100 "$url" "$CA" \
    "$ROOT/$user.credentials" crossnook-test-device \
    crossnook-test-device-id "$epoch" "$recv_ms"
}

expect_success() {
  name=$1
  expected=$2
  shift 2
  output=$("$@")
  printf "%s: %s\n" "$name" "$output"
  printf "%s\n" "$output" | grep -q "$expected"
}

expect_failure() {
  name=$1
  expected=$2
  shift 2
  set +e
  output=$("$@" 2>&1)
  status=$?
  set -e
  printf "%s: %s\n" "$name" "$output"
  test "$status" -ne 0
  printf "%s\n" "$output" | grep -q "$expected"
}

wait_for_fixed() {
  expected=$1
  file=$2
  attempts=0
  while ! grep -Fq "$expected" "$file" 2>/dev/null; do
    attempts=$((attempts + 1))
    test "$attempts" -lt 20
    sleep 0.25
  done
}

echo "--- directly relevant protocol regression ---"
$Q $BIN --api-smoke
$Q $BIN --policy-smoke
$Q $BIN --mock-smoke http://host.docker.internal:18080
$Q $BIN --roundtrip http://host.docker.internal:18080 test-user "$KEY" \
  e1a1e9016cfc9bca8c694187943e9c4f 519220cea448409961e6b3081a36eca3
$Q $BIN --network-error http://127.0.0.2:1

echo "--- focused end-to-end integration matrix ---"
for user in integration-local-only integration-remote-only \
  integration-both-missing integration-same \
  integration-same-percentage-different \
  integration-different-same-percentage integration-different \
  integration-unsupported integration-auth integration-malformed \
  integration-timeout integration-put-timeout; do
  make_credentials "$user"
  mkdir -p "$ROOT/$user"
done

$Q $BIN --local-set "$ROOT/integration-local-only" "$EPUB" \
  "/body/DocFragment[1]/body/p[1]/text().0" 3210
expect_success local-present-remote-missing \
  "status=ok decision=local-selected local=1 remote=0 saved=0 uploaded=1 document=e1a1e9016cfc9bca8c694187943e9c4f.*local-save-attempted=0 remote-put-attempted=1 outcome=uploaded retry=none local-mutation=none remote-mutation=confirmed" \
  sync_once integration-local-only "$ROOT/integration-local-only"

expect_success remote-present-local-missing \
  "status=ok decision=remote-selected local=0 remote=1 saved=1 uploaded=0.*local-save-attempted=1 remote-put-attempted=0 outcome=imported retry=none local-mutation=confirmed remote-mutation=none" \
  sync_once integration-remote-only "$ROOT/integration-remote-only"
expect_success remote-persisted-unchanged \
  "progress=6543 position=/body/DocFragment\[1\]/body/p\[3\]/text().5 OK" \
  $Q $BIN --local-get "$ROOT/integration-remote-only" "$EPUB"

expect_success both-missing \
  "status=ok decision=no-state local=0 remote=0 saved=0 uploaded=0.*local-save-attempted=0 remote-put-attempted=0 outcome=no-state retry=none local-mutation=none remote-mutation=none" \
  sync_once integration-both-missing "$ROOT/integration-both-missing"

expect_success caller-established-wall-clock \
  "status=ok decision=no-state.*time=ok dns=ok kosync=not-found.*transport=ok" \
  sync_once integration-both-missing "$ROOT/integration-both-missing" \
    "$URL" "$DNS_PORT" caller-established -

$Q $BIN --local-set "$ROOT/integration-same" "$EPUB" \
  "/body/DocFragment[1]/body/p[1]/text().0" 3210
expect_success same-position-same-percentage \
  "status=ok decision=no-change local=1 remote=1 saved=0 uploaded=0.*local-save-attempted=0 remote-put-attempted=0 outcome=unchanged retry=none local-mutation=none remote-mutation=none" \
  sync_once integration-same "$ROOT/integration-same"

$Q $BIN --local-set "$ROOT/integration-same-percentage-different" "$EPUB" \
  "/body/DocFragment[1]/body/p[1]/text().0" 3210
expect_success same-position-different-percentage \
  "status=ok decision=no-change local=1 remote=1 saved=0 uploaded=0.*local-save-attempted=0 remote-put-attempted=0 outcome=unchanged retry=none local-mutation=none remote-mutation=none" \
  sync_once integration-same-percentage-different \
    "$ROOT/integration-same-percentage-different"

$Q $BIN --local-set "$ROOT/integration-different-same-percentage" "$EPUB" \
  "/body/DocFragment[1]/body/p[1]/text().0" 3210
expect_success different-position-same-percentage \
  "status=ok decision=ambiguous local=1 remote=1 saved=0 uploaded=0.*local-save-attempted=0 remote-put-attempted=0 outcome=conflict retry=explicit-action local-mutation=none remote-mutation=none" \
  sync_once integration-different-same-percentage \
    "$ROOT/integration-different-same-percentage"
expect_success ambiguous-local-unchanged \
  "progress=3210 position=/body/DocFragment\[1\]/body/p\[1\]/text().0 OK" \
  $Q $BIN --local-get "$ROOT/integration-different-same-percentage" "$EPUB"

$Q $BIN --local-set "$ROOT/integration-different" "$EPUB" \
  "/body/DocFragment[1]/body/p[1]/text().0" 3210
expect_success different-position-different-percentage \
  "status=ok decision=ambiguous local=1 remote=1 saved=0 uploaded=0.*local-save-attempted=0 remote-put-attempted=0 outcome=conflict retry=explicit-action local-mutation=none remote-mutation=none" \
  sync_once integration-different "$ROOT/integration-different"

$Q $BIN --local-set "$ROOT/integration-unsupported" "$EPUB" \
  "/body/DocFragment[1]/body/p[1]/text().0" -1
expect_failure local-unknown-percentage \
  "status=local-unsupported decision=local-selected local=1 remote=0.*load=ok.*local-save-attempted=0 remote-put-attempted=0 outcome=local-unsupported retry=explicit-action local-mutation=none remote-mutation=none" \
  sync_once integration-unsupported "$ROOT/integration-unsupported"

make_credentials integration-auth wrong-key
expect_failure authentication-failure \
  "status=auth-failed.*kosync=auth-failed http=401 transport=ok.*local-save-attempted=0 remote-put-attempted=0 outcome=auth-required retry=explicit-action local-mutation=none remote-mutation=none" \
  sync_once integration-auth "$ROOT/integration-auth"

expect_failure dns-failure \
  "status=dns-failed.*dns=nxdomain.*outcome=service-failure retry=explicit-action" \
  sync_once integration-both-missing "$ROOT/integration-both-missing" \
    "$URL" $((DNS_PORT + 4))

expect_failure trusted-time-failure \
  "status=trusted-time-failed.*time=invalid dns=invalid.*outcome=trusted-time-unavailable retry=explicit-action" \
  sync_once integration-both-missing "$ROOT/integration-both-missing" \
    "$URL" "$DNS_PORT" sync - 1000 not-an-ip

expect_failure wrong-tls-hostname \
  "status=https-failed.*transport=tls-hostname-mismatch.*outcome=security-failure retry=explicit-action.*remote-mutation=none" \
  sync_once integration-both-missing "$ROOT/integration-both-missing" \
    "$WRONG_URL"

expect_failure malformed-kosync-response \
  "status=protocol-failed.*kosync=bad-json http=200 transport=ok.*outcome=service-failure retry=explicit-action" \
  sync_once integration-malformed "$ROOT/integration-malformed"

expect_failure transport-receive-timeout \
  "status=https-failed.*kosync=transport-error.*transport=tls-recv-timeout.*outcome=connectivity-failure retry=automatic-later" \
  sync_once integration-timeout "$ROOT/integration-timeout" \
    "$URL" "$DNS_PORT" caller-established "$EPOCH" 200

$Q $BIN --local-set "$ROOT/integration-put-timeout" "$EPUB" \
  "/body/DocFragment[1]/body/p[1]/text().0" 3210
expect_failure uncertain-put-result \
  "status=https-failed decision=local-selected local=1 remote=0 saved=0 uploaded=0.*transport=tls-recv-timeout.*local-save-attempted=0 remote-put-attempted=1 outcome=connectivity-failure retry=automatic-later local-mutation=none remote-mutation=possible" \
  sync_once integration-put-timeout "$ROOT/integration-put-timeout" \
    "$URL" "$DNS_PORT" caller-established "$EPOCH" 200
expect_success uncertain-put-was-stored \
  "status=ok decision=no-change local=1 remote=1 saved=0 uploaded=0.*local-save-attempted=0 remote-put-attempted=0 outcome=unchanged retry=none local-mutation=none remote-mutation=none" \
  sync_once integration-put-timeout "$ROOT/integration-put-timeout"

echo "--- redacted transcript and hostname identity ---"
PUT_EVENT='{"document":"e1a1e9016cfc9bca8c694187943e9c4f","method":"PUT","percentage":0.321,"progress":"/body/DocFragment[1]/body/p[1]/text().0","username":"integration-local-only"}'
UNCERTAIN_PUT_EVENT='{"document":"e1a1e9016cfc9bca8c694187943e9c4f","method":"PUT","percentage":0.321,"progress":"/body/DocFragment[1]/body/p[1]/text().0","username":"integration-put-timeout"}'
# Windows host writes these files while the container reads the bind mount.
wait_for_fixed "$PUT_EVENT" /io/work/crossnook-kosync-integration.jsonl
wait_for_fixed "$UNCERTAIN_PUT_EVENT" /io/work/crossnook-kosync-integration.jsonl
wait_for_fixed secure.test.local /io/work/crossnook-kosync-sni.log
wait_for_fixed secure.test.local:19443 /io/work/crossnook-kosync-host.log
wait_for_fixed wrong.test /io/work/crossnook-kosync-sni.log
test "$(grep -c '"method":"PUT"' /io/work/crossnook-kosync-integration.jsonl)" -eq 2
test -z "$(grep -i 'auth-key\|dfb450efddbb5387197c84460623675b\|wrong-key' \
  /io/work/crossnook-kosync-integration.jsonl || true)"
grep -Fxq secure.test.local /io/work/crossnook-kosync-sni.log
grep -Fxq secure.test.local:19443 /io/work/crossnook-kosync-host.log
grep -Fxq wrong.test /io/work/crossnook-kosync-sni.log

echo "--- insecure integration transport rejected ---"
expect_failure insecure-http "insecure-http-rejected" \
  $Q $BIN --sync-once "$ROOT/integration-both-missing" "$EPUB" \
    "$HOST_IP" "$DNS_PORT" 300 caller-established 127.0.0.2 19123 100 \
    http://secure.test.local:19443 "$CA" \
    "$ROOT/integration-both-missing.credentials" crossnook-test-device \
    crossnook-test-device-id "$EPOCH" 1000

echo "KOSYNC INTEGRATION HOST VALIDATION OK"
