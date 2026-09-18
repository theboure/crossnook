#!/usr/bin/env bash
# Build + host-validate crossnook-test with the pinned musl toolchain.
# Requires the toolchain image from toolchain/build-smoke.sh (crossnook-toolchain).
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

echo "==> cross-compiling crossnook-test (static, EABI5 soft-float, non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io/testapp \
  "$IMG" bash -c '
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra \
        -o crossnook-test crossnook-test.c
    echo "--- file ---"
    file crossnook-test
    echo "--- readelf -h ---"
    readelf -h crossnook-test | grep -E "Type|Machine|Flags|Entry"
    echo "--- fully static? ---"
    readelf -l crossnook-test | grep -E "INTERP|Dynamic" && \
        echo "WARNING: dynamic sections present" || \
        echo "no INTERP/Dynamic -> fully static, non-PIE"
    echo "--- attributes ---"
    readelf -A crossnook-test | grep -iE "Float|VFP|Tag_CPU_arch|Tag_ABI_VFP_args" || true
    echo "--- host-side self-check via qemu-arm ---"
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi ./crossnook-test --selfcheck /io/testapp/fb-selfcheck.bin
    echo "--- selfcheck fb file size ---"
    ls -la /io/testapp/fb-selfcheck.bin
  '

echo "==> artifacts"
ls -la "$REPO/testapp/crossnook-test"
sha256sum "$REPO/testapp/crossnook-test"