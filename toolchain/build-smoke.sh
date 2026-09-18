#!/usr/bin/env bash
# One-step: build toolchain image, cross-compile smoke test, host-validate.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

echo "==> building toolchain image"
docker build --quiet -t "$IMG" -f "$REPO/toolchain/Dockerfile" "$REPO" || { echo "docker build failed"; exit 1; }

echo "==> cross-compiling smoke test (static, EABI5 soft-float)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io/toolchain/smoke \
  "$IMG" bash -c '
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -o smoke smoke.c
    echo "--- gcc version ---"
    arm-linux-musleabi-gcc --version | head -1
    echo "--- musl version ---"
    arm-linux-musleabi-gcc -dM -E -xc /dev/null | grep -E "__MUSL" || echo "(no __MUSL defines)"
    echo "--- file ---"
    file smoke
    echo "--- readelf -h ---"
    readelf -h smoke | grep -E "Type|Machine|Flags|Entry"
    echo "--- readelf -l (dynamic section present?) ---"
    readelf -l smoke | grep -E "INTERP|Dynamic" || echo "no INTERP/Dynamic -> fully static, non-PIE"
    echo "--- attributes (float ABI) ---"
    readelf -A smoke | grep -iE "Float|VFP|Tag_CPU_arch|Tag_ABI_VFP_args" || true
    echo "--- run via qemu-arm ---"
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi ./smoke
  '

echo "==> artifacts"
ls -la "$REPO/toolchain/smoke/smoke"
sha256sum "$REPO/toolchain/smoke/smoke"