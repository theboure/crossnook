#!/usr/bin/env bash
# Rebuild the pinned BearSSL sources with the ARM compiler as a focused gate.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG="crossnook-toolchain"

bash "$REPO/toolchain/bearssl/fetch.sh"
MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO_WIN:/io" "$IMG" bash -c '
  set -euo pipefail
  export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
  rm -rf /tmp/bearssl-src /tmp/bearssl-obj /io/work/bearssl-check
  mkdir -p /tmp/bearssl-src /tmp/bearssl-obj
  tar -xJf /io/work/bearssl/bearssl_0.6+dfsg.1.orig.tar.xz \
    -C /tmp/bearssl-src --strip-components=1
  cd /tmp/bearssl-obj
  for source in $(find /tmp/bearssl-src/src -name "*.c" | sort); do
    name=$(printf "%s" "$source" | sha256sum | cut -c1-20)
    arm-linux-musleabi-gcc -std=c99 -O2 -Wall -Wextra \
      -I/tmp/bearssl-src/inc -I/tmp/bearssl-src/src \
      -c "$source" -o "$name.o"
  done
  count=$(find . -name "*.o" | wc -l)
  test "$count" -gt 200
  mkdir -p /io/work/bearssl-check/include
  arm-linux-musleabi-ar rcs /io/work/bearssl-check/libbearssl.a ./*.o
  cp /tmp/bearssl-src/inc/bearssl.h /tmp/bearssl-src/inc/bearssl_*.h \
    /io/work/bearssl-check/include/
  test -s /io/work/bearssl-check/libbearssl.a
  echo "BEARSSL PINNED ARM REBUILD OK objects=$count"
  rm -rf /tmp/bearssl-src /tmp/bearssl-obj
'
