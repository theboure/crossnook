#!/usr/bin/env bash
# Build and host-validate KOReader-compatible document identities.
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
else echo "python is required to generate EPUB fixtures"; exit 1
fi

echo "==> regenerating deterministic EPUB fixtures"
bash "$REPO/testapp/cre-fixtures/make-fixtures.sh"
"$PY" "$REPO/testapp/position-fixtures/make-foreign.py" \
  "$REPO/testapp/position-fixtures/foreign.epub"

echo "==> cross-compiling crossnook-bookid-test (static EABI5 non-PIE)"
MSYS_NO_PATHCONV=1 docker run --rm \
  -v "$REPO_WIN:/io" \
  -w /io \
  "$IMG" bash -c '
    set -euo pipefail
    export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
    mkdir -p /o /tmp/crossnook-bookid-fixtures
    cd /o

    gcc -std=c11 -O2 -Wall -Wextra \
      /io/testapp/bookid-fixtures/make-fixtures.c -o make-bookid-fixtures
    ./make-bookid-fixtures /tmp/crossnook-bookid-fixtures
    cp /io/testapp/cre-fixtures/test.epub \
      /tmp/crossnook-bookid-fixtures/test.epub
    cp /io/testapp/reader-fixtures/valid2.epub \
      /tmp/crossnook-bookid-fixtures/valid2.epub
    cp /io/testapp/position-fixtures/foreign.epub \
      /tmp/crossnook-bookid-fixtures/foreign.epub

    CFLAGS="-std=c11 -static -no-pie -fno-pie -O2 -Wall -Wextra -I/io/src"
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/app/bookid-test.c -o bookid-test.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/book/koreader_identity.c -o koreader-identity.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/book/md5.c -o md5.o
    arm-linux-musleabi-gcc $CFLAGS \
      -c /io/src/progress/book_identity.c -o local-identity.o
    arm-linux-musleabi-gcc -static -no-pie -fno-pie -O2 -Wall -Wextra \
      bookid-test.o koreader-identity.o md5.o local-identity.o \
      -o /io/testapp/crossnook-bookid-test

    echo "--- file ---"
    file /io/testapp/crossnook-bookid-test
    file /io/testapp/crossnook-bookid-test | \
      grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
    readelf -h /io/testapp/crossnook-bookid-test | grep -E "Type|Machine|Flags|Entry"
    readelf -h /io/testapp/crossnook-bookid-test | grep -q "Type:.*EXEC"
    readelf -h /io/testapp/crossnook-bookid-test | grep -q "Machine:.*ARM"
    readelf -h /io/testapp/crossnook-bookid-test | \
      grep -q "Flags:.*Version5 EABI.*soft-float ABI"
    readelf -l /io/testapp/crossnook-bookid-test | grep -E "INTERP|DYNAMIC" \
      && { echo "FAIL: dynamic sections present"; exit 1; } \
      || echo "no INTERP/Dynamic -> fully static, non-PIE"

    echo "--- structural separation ---"
    test -z "$(grep -l "koreader_identity\|cn_book_identity_koreader" \
      /io/src/ui/ui.c /io/src/reader/reader.cpp \
      /io/src/progress/progress_store.c /io/src/library/library.c || true)"
    echo "OK: KOReader identity remains isolated from runtime layers"

    Q="qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi"
    echo "--- API and Local Progress identity smoke ---"
    $Q /io/testapp/crossnook-bookid-test --api-smoke

    echo "--- pinned KOReader Binary vectors ---"
    while read -r name expected; do
      case "$name" in ""|\#*) continue ;; esac
      actual=$($Q /io/testapp/crossnook-bookid-test --binary \
        "/tmp/crossnook-bookid-fixtures/$name")
      test "$actual" = "BOOKID binary $expected" || {
        echo "FAIL: $name expected=$expected actual=$actual"
        exit 1
      }
      echo "[OK] $name $expected"
    done < /io/testapp/bookid-vectors.txt

    echo "--- rename and repeat invariance ---"
    mkdir -p /tmp/crossnook-bookid-a /tmp/crossnook-bookid-b
    cp /io/testapp/cre-fixtures/test.epub \
      /tmp/crossnook-bookid-a/original.epub
    cp /io/testapp/cre-fixtures/test.epub \
      /tmp/crossnook-bookid-b/renamed.bin
    first=$($Q /io/testapp/crossnook-bookid-test --binary \
      /tmp/crossnook-bookid-a/original.epub)
    second=$($Q /io/testapp/crossnook-bookid-test --binary \
      /tmp/crossnook-bookid-b/renamed.bin)
    repeat=$($Q /io/testapp/crossnook-bookid-test --binary \
      /tmp/crossnook-bookid-a/original.epub)
    test "$first" = "$second" && test "$first" = "$repeat"
    echo "[OK] same bytes ignore path and filename; repeated result is stable"

    echo "--- exact KOReader Filename vectors ---"
    test "$($Q /io/testapp/crossnook-bookid-test --filename /books/Book.EPUB)" = \
      "BOOKID filename a2cc79d135ccff15ce033aad010dd157"
    test "$($Q /io/testapp/crossnook-bookid-test --filename /books/book.epub)" = \
      "BOOKID filename 03053ffc045564439ff7f2cabb3b58c5"
    test "$($Q /io/testapp/crossnook-bookid-test --filename /books/Book)" = \
      "BOOKID filename 2b1f94ef23b79bf90eb891cae1df7a90"
    test "$($Q /io/testapp/crossnook-bookid-test --filename "C:\books\Book.EPUB")" = \
      "BOOKID filename 36303671a61d167cb42170bf5045eabc"
    echo "[OK] basename, extension, case, and slash semantics match KOReader"

    echo "--- failures ---"
    if $Q /io/testapp/crossnook-bookid-test --binary \
         /tmp/crossnook-bookid-fixtures/missing.epub; then
      echo "FAIL: missing file accepted"; exit 1
    fi
    if $Q /io/testapp/crossnook-bookid-test --binary \
         /tmp/crossnook-bookid-fixtures; then
      echo "FAIL: directory accepted as a readable file"; exit 1
    fi
    echo "[OK] missing and unreadable inputs fail safely"
    echo "BOOKID HOST VALIDATION OK"
  '

echo "==> artifact"
ls -la "$REPO/testapp/crossnook-bookid-test"
sha256sum "$REPO/testapp/crossnook-bookid-test"
