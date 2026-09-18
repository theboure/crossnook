#!/usr/bin/env bash
# Build + host-validate the Reader Logical Position / Sync Foundation.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || echo "$REPO")
IMG="crossnook-toolchain"

if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  echo "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi

if command -v python >/dev/null 2>&1; then PY=python
elif command -v python3 >/dev/null 2>&1; then PY=python3
else echo "python is required to generate position fixtures"; exit 1
fi

echo "==> regenerating deterministic position fixtures"
bash "$REPO/testapp/cre-fixtures/make-fixtures.sh"
"$PY" "$REPO/testapp/position-fixtures/make-foreign.py" \
  "$REPO/testapp/position-fixtures/foreign.epub"
sha256sum "$REPO/testapp/position-fixtures/foreign.epub"

echo "==> cross-compiling crossnook-position-test (static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o && cd /o

    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra \
      -I/io/src -c /io/src/app/reader-position-test.c -o position-test.o
    arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie \
      -O2 -Wall -Wextra -include stdint.h \
      -I/io/src -I/io/src/reader \
      -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
      -c /io/src/reader/reader.cpp -o reader.o
    arm-linux-musleabi-g++ -static -no-pie -fno-pie -O2 -Wall -Wextra \
      position-test.o reader.o -o /io/testapp/crossnook-position-test \
      /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
      /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm

    echo "--- file ---"
    file /io/testapp/crossnook-position-test
    readelf -h /io/testapp/crossnook-position-test | grep -E "Type|Machine|Flags|Entry"
    readelf -l /io/testapp/crossnook-position-test | grep -E "INTERP|Dynamic" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- structural: CREngine remains private to reader.cpp ---"
    CRE_USERS=$(grep -rl "lvdocview.h\|lvtinydom.h" /io/src --include="*.c" --include="*.cpp" || true)
    test "$CRE_USERS" = "/io/src/reader/reader.cpp"
    echo "OK: $CRE_USERS"

    echo "--- logical position smoke under qemu-arm ---"
    qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
      /io/testapp/crossnook-position-test --smoke \
      /io/testapp/test-font.ttf \
      /io/testapp/cre-fixtures/test.epub \
      /io/testapp/position-fixtures/foreign.epub
  '

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-position-test"
sha256sum "$REPO/testapp/crossnook-position-test"
