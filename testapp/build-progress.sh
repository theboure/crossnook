#!/usr/bin/env bash
# Build + host-validate local logical reading progress persistence.
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
else echo "python is required to generate progress fixtures"; exit 1
fi

echo "==> regenerating deterministic progress fixtures"
bash "$REPO/testapp/cre-fixtures/make-fixtures.sh"
"$PY" "$REPO/testapp/position-fixtures/make-foreign.py" \
  "$REPO/testapp/position-fixtures/foreign.epub"

echo "==> cross-compiling crossnook-progress-test (static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o && cd /o

    CFLAGS="-static -no-pie -fno-pie -O2 -Wall -Wextra -I/io/src"
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/app/progress-store-test.c -o progress-test.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/progress/book_identity.c -o book-identity.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/progress/progress_store.c -o progress-store.o
    arm-linux-musleabi-g++ -std=c++17 -static -no-pie -fno-pie \
      -O2 -Wall -Wextra -include stdint.h \
      -I/io/src -I/io/src/reader \
      -I/opt/crengine/include -I/opt/freetype/include/freetype2 \
      -c /io/src/reader/reader.cpp -o reader.o
    arm-linux-musleabi-g++ -static -no-pie -fno-pie -O2 -Wall -Wextra \
      progress-test.o book-identity.o progress-store.o reader.o \
      -o /io/testapp/crossnook-progress-test \
      /opt/crengine/lib/libcrengine.a /opt/freetype/lib/libfreetype.a \
      /opt/zlib/lib/libz.a /opt/xxhash/lib/libxxhash.a -lm

    echo "--- file ---"
    file /io/testapp/crossnook-progress-test
    readelf -h /io/testapp/crossnook-progress-test | grep -E "Type|Machine|Flags|Entry"
    readelf -l /io/testapp/crossnook-progress-test | grep -E "INTERP|Dynamic" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- structural: persistence remains outside UI/Reader/Library ---"
    test -z "$(grep -l "progress_store\|CN_PROGRESS" \
      /io/src/ui/ui.c /io/src/reader/reader.cpp /io/src/library/library.c || true)"
    echo "OK: persistence policy is application-owned"

    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    rm -rf /tmp/crossnook-progress-errors
    mkdir -p /tmp/crossnook-progress-errors
    echo "--- corruption and error validation ---"
    $Q /io/testapp/crossnook-progress-test --errors \
      /io/testapp/test-font.ttf \
      /io/testapp/cre-fixtures/test.epub \
      /tmp/crossnook-progress-errors

    rm -rf /tmp/crossnook-progress-restart
    mkdir -p /tmp/crossnook-progress-restart
    echo "--- process A: save two books ---"
    $Q /io/testapp/crossnook-progress-test --save \
      /io/testapp/test-font.ttf \
      /io/testapp/cre-fixtures/test.epub \
      /io/testapp/reader-fixtures/valid2.epub \
      /io/testapp/position-fixtures/foreign.epub \
      /tmp/crossnook-progress-restart
    echo "--- process B: restore two books and relayout ---"
    $Q /io/testapp/crossnook-progress-test --restore \
      /io/testapp/test-font.ttf \
      /io/testapp/cre-fixtures/test.epub \
      /io/testapp/reader-fixtures/valid2.epub \
      /io/testapp/position-fixtures/foreign.epub \
      /tmp/crossnook-progress-restart
  '

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-progress-test"
sha256sum "$REPO/testapp/crossnook-progress-test"
