#!/usr/bin/env bash
# Build and host-validate KOSync protocol plus one-shot sync integration.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG="crossnook-toolchain"
DNS_BASE=19353
HTTPS_PORT=19443
PIDS=""
TRANSCRIPT="work/crossnook-kosync-integration.jsonl"
SNI_LOG="work/crossnook-kosync-sni.log"
HOST_LOG="work/crossnook-kosync-host.log"

cleanup() {
  for pid in $PIDS; do
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
}
trap cleanup EXIT

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  printf '%s\n' "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

cd "$REPO"
mkdir -p work
rm -f "$TRANSCRIPT" "$SNI_LOG" "$HOST_LOG"

echo "==> validating deterministic Python mock server"
python testapp/kosync_mock_server.py --self-test

set -- $(MSYS_NO_PATHCONV=1 docker run --rm \
  --add-host=host.docker.internal:host-gateway \
  "$IMG" getent ahostsv4 host.docker.internal)
HOST_IP=$1

python testapp/kosync_mock_server.py --host 0.0.0.0 --port 18080 \
  --timestamp-start 1700000000 --quiet >work/crossnook-kosync-mock.log 2>&1 &
PIDS="$PIDS $!"
python testapp/dns_mock_server.py --host 0.0.0.0 --port "$DNS_BASE" \
  --answer "$HOST_IP" --suite >work/crossnook-kosync-dns.log 2>&1 &
PIDS="$PIDS $!"
python testapp/https_mock_server.py --host 0.0.0.0 --port "$HTTPS_PORT" \
  --cert testapp/pki/server.crt --key testapp/pki/server.key \
  --timestamp-start 1700000000 --integration-fixtures \
  --kosync-stall-seconds 1 --transcript "$TRANSCRIPT" \
  --sni-log "$SNI_LOG" --host-log "$HOST_LOG" --quiet \
  >work/crossnook-kosync-https.log 2>&1 &
PIDS="$PIDS $!"
sleep 1
for pid in $PIDS; do kill -0 "$pid"; done

echo "==> cross-compiling integrated crossnook-kosync-test"
MSYS_NO_PATHCONV=1 docker run --rm \
  --add-host=host.docker.internal:host-gateway \
  -v "$REPO_WIN:/io" -w /io "$IMG" \
  bash /io/testapp/run-kosync-integration.sh

echo "==> artifact"
ls -la testapp/crossnook-kosync-test
sha256sum testapp/crossnook-kosync-test
