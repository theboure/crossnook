#!/usr/bin/env bash
# Build and host-validate verified HTTPS plus KOSync-over-HTTPS.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
PKI="$REPO/testapp/pki"
IMG="crossnook-toolchain"
PIDS=""

cleanup() {
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

echo "==> inspecting TEST-ONLY PKI"
sha256sum "$PKI"/* > /tmp/crossnook-pki-before.sha256
bash "$REPO/testapp/gen-pki.sh" >/tmp/crossnook-gen-pki.log 2>&1
sha256sum "$PKI"/* > /tmp/crossnook-pki-after.sha256
diff -u /tmp/crossnook-pki-before.sha256 /tmp/crossnook-pki-after.sha256
openssl pkey -in "$PKI/server.key" -check -noout
openssl x509 -in "$PKI/server.crt" -noout -dates -ext subjectAltName
openssl verify -attime 1789948800 -CAfile "$PKI/testca.crt" "$PKI/server.crt"
openssl verify -attime 1789948800 -CAfile "$PKI/testca.crt" \
  -untrusted "$PKI/intermediate.crt" \
  "$PKI/missing-chain.crt"

bash "$REPO/toolchain/bearssl/check-build.sh"

bash "$REPO/testapp/build-nettest.sh"
bash "$REPO/testapp/build-kosync.sh"

rm -f /tmp/crossnook-https-sni.log /tmp/crossnook-https-*.log

python "$REPO/testapp/https_mock_server.py" --host 0.0.0.0 --port 18443 \
  --cert "$PKI/server.crt" --key "$PKI/server.key" \
  --sni-log /tmp/crossnook-https-sni.log --quiet \
  >/tmp/crossnook-https-nominal.log 2>&1 &
PIDS="$PIDS $!"
python "$REPO/testapp/https_mock_server.py" --host 0.0.0.0 --port 18444 \
  --cert "$PKI/expired.crt" --key "$PKI/server.key" --quiet \
  >/tmp/crossnook-https-expired.log 2>&1 &
PIDS="$PIDS $!"
python "$REPO/testapp/https_mock_server.py" --host 0.0.0.0 --port 18445 \
  --cert "$PKI/notyet.crt" --key "$PKI/server.key" --quiet \
  >/tmp/crossnook-https-notyet.log 2>&1 &
PIDS="$PIDS $!"
python "$REPO/testapp/https_mock_server.py" --host 0.0.0.0 --port 18446 \
  --cert "$PKI/missing-chain.crt" --key "$PKI/missing-chain.key" --quiet \
  >/tmp/crossnook-https-chain.log 2>&1 &
PIDS="$PIDS $!"
python "$REPO/testapp/https_mock_server.py" --host 0.0.0.0 --port 18447 \
  --mode plain --quiet >/tmp/crossnook-https-plain.log 2>&1 &
PIDS="$PIDS $!"
python "$REPO/testapp/https_mock_server.py" --host 0.0.0.0 --port 18448 \
  --mode handshake-stall --stall-seconds 2 --quiet \
  >/tmp/crossnook-https-stall.log 2>&1 &
PIDS="$PIDS $!"
sleep 1

echo "==> ARM HTTPS integration under qemu"
MSYS_NO_PATHCONV=1 docker run --rm \
  --add-host=host.docker.internal:host-gateway \
  -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
    set -euo pipefail
    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    NET="/io/testapp/crossnook-net-test"
    KOSYNC="/io/testapp/crossnook-kosync-test"
    CA="/io/testapp/pki/testca.crt"
    OTHER="/io/testapp/pki/otherca.crt"
    NOW=1789948800

    expect_fail() {
      expected=$1
      shift
      set +e
      output=$("$@" 2>&1)
      status=$?
      set -e
      printf "%s\n" "$output"
      test "$status" -ne 0
      printf "%s\n" "$output" | grep -q "$expected"
    }

    echo "--- verified HTTPS GET and PUT ---"
    $Q $NET --https-get host.docker.internal 18443 secure.test.local \
      $CA /https/get $NOW
    $Q $NET --https-put host.docker.internal 18443 secure.test.local \
      $CA /https/put test-body $NOW

    echo "--- identity, trust, time, chain, and handshake rejection ---"
    expect_fail tls-hostname-mismatch $Q $NET --https-get \
      host.docker.internal 18443 wrong.test $CA /https/get $NOW
    expect_fail tls-trust-failed $Q $NET --https-get \
      host.docker.internal 18443 secure.test.local $OTHER /https/get $NOW
    expect_fail tls-cert-time-failed $Q $NET --https-get \
      host.docker.internal 18444 secure.test.local $CA /https/get $NOW
    expect_fail tls-cert-time-failed $Q $NET --https-get \
      host.docker.internal 18445 secure.test.local $CA /https/get $NOW
    expect_fail tls-trust-failed $Q $NET --https-get \
      host.docker.internal 18446 secure.test.local $CA /https/get $NOW
    expect_fail tls-handshake-failed $Q $NET --https-get \
      host.docker.internal 18447 secure.test.local $CA /https/get $NOW
    expect_fail tls-handshake-timeout $Q $NET --https-get \
      host.docker.internal 18448 secure.test.local $CA /https/get $NOW 300

    echo "--- bounded response and read timeout ---"
    expect_fail truncated $Q $NET --https-get \
      host.docker.internal 18443 secure.test.local $CA /https/large $NOW
    expect_fail tls-recv-timeout $Q $NET --https-get \
      host.docker.internal 18443 secure.test.local $CA /https/stall $NOW 300
    expect_fail truncated $Q $NET --https-get \
      host.docker.internal 18443 secure.test.local $CA /https/short-body $NOW
    expect_fail truncated $Q $NET --https-get \
      host.docker.internal 18443 secure.test.local $CA /https/short-headers $NOW
    expect_fail tls-protocol-failed $Q $NET --https-get \
      host.docker.internal 18443 secure.test.local $CA /https/raw-eof $NOW
    $Q $NET --tls-write-timeout host.docker.internal 18443 \
      secure.test.local $CA $NOW 300

    echo "--- KOSync round-trip over verified HTTPS ---"
    $Q $KOSYNC --roundtrip-https https://secure.test.local:18443 \
      host.docker.internal $CA test-user dfb450efddbb5387197c84460623675b \
      e1a1e9016cfc9bca8c694187943e9c4f \
      519220cea448409961e6b3081a36eca3 $NOW
  '

grep -qx 'secure.test.local' /tmp/crossnook-https-sni.log
echo "SNI secure.test.local -> OK"
echo "HTTPS HOST VALIDATION OK"
