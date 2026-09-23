#!/usr/bin/env bash
# Focused synthetic credential-store validation; no workstation credentials.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
REPO_WIN=$(cygpath -w "$REPO" 2>/dev/null || printf '%s' "$REPO")
IMG=crossnook-toolchain
if ! docker image inspect "$IMG" >/dev/null 2>&1; then
  printf '%s\n' "toolchain image missing; run: bash toolchain/build-smoke.sh"
  exit 1
fi
MSYS_NO_PATHCONV=1 docker run --rm -v "$REPO_WIN:/io" -w /io "$IMG" bash -c '
  set -euo pipefail
  export PATH=/opt/tc/arm-linux-musleabi-cross/bin:$PATH
  mkdir -p /o
  cd /o
  CFLAGS="-std=c11 -D_POSIX_C_SOURCE=200809L -static -no-pie -fno-pie -march=armv5te -O2 -Wall -Wextra -Werror -I/io/src"
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/credentials/credential_store.c -o credential-store.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/app/credential-store-test.c -o credential-store-test.o
  arm-linux-musleabi-gcc $CFLAGS -c /io/src/platform/storage_verify.c -o storage-verify.o
  if arm-linux-musleabi-nm -u credential-store.o | grep -E " (malloc|calloc|realloc|free)$"; then
    echo "FAIL: credential store references heap allocation"
    exit 1
  fi
  arm-linux-musleabi-gcc -static -no-pie -fno-pie -march=armv5te -O2 \
    credential-store.o credential-store-test.o storage-verify.o \
    -Wl,--wrap=open -Wl,--wrap=write -Wl,--wrap=fsync \
    -Wl,--wrap=close -Wl,--wrap=rename \
    -o /io/testapp/crossnook-credentials-test
  file /io/testapp/crossnook-credentials-test | grep -q "ELF 32-bit LSB executable, ARM.*statically linked"
  readelf -h /io/testapp/crossnook-credentials-test | grep -q "Type:.*EXEC"
  readelf -h /io/testapp/crossnook-credentials-test | grep -q "Machine:.*ARM"
  readelf -h /io/testapp/crossnook-credentials-test | grep -q "Flags:.*Version5 EABI.*soft-float ABI"
  readelf -A /io/testapp/crossnook-credentials-test | grep -q "Tag_CPU_arch: v5TE"
  if readelf -l /io/testapp/crossnook-credentials-test | grep -E "INTERP|DYNAMIC"; then
    echo "FAIL: dynamic sections present"
    exit 1
  fi
  echo "ARM static EABI5 soft-float non-PIE -> OK"
  ROOT=$(mktemp -d /tmp/crossnook-credentials-host.XXXXXX)
  trap "rm -rf \"\$ROOT\"" EXIT
  mkdir "$ROOT/allowed"
  printf "%s\n" "unchanged-outside" > "$ROOT/sentinel"
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-credentials-test --smoke "$ROOT/allowed" \
    > "$ROOT/output" 2>&1
  if [ "$(cat "$ROOT/sentinel")" != "unchanged-outside" ]; then
    echo "FAIL: outside-directory sentinel changed"
    exit 1
  fi
  if grep -F -e "crossnook-synthetic-account-only" \
     -e "crossnook-synthetic-nonreal-key-only" \
     -e "crossnook-synthetic-alternate-only" "$ROOT/output"; then
    echo "FAIL: diagnostic exposed synthetic credential"
    exit 1
  fi
  qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
    /io/testapp/crossnook-credentials-test --check-output "$ROOT/output" \
    > "$ROOT/redaction-ok" 2>&1
  if qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
       /io/testapp/crossnook-credentials-test --check-output \
       "$ROOT/allowed/credentials" > "$ROOT/redaction-negative" 2>&1; then
    echo "FAIL: diagnostic missed synthetic credential in captured file"
    exit 1
  fi
  if grep -F -e "crossnook-synthetic-account-only" \
     -e "crossnook-synthetic-nonreal-key-only" \
     -e "crossnook-synthetic-alternate-only" \
     "$ROOT/redaction-ok" "$ROOT/redaction-negative"; then
    echo "FAIL: redaction check exposed synthetic credential"
    exit 1
  fi
  if grep -F -e "crossnook-synthetic-account-only" \
     -e "crossnook-synthetic-nonreal-key-only" \
     -e "crossnook-synthetic-alternate-only" \
     /io/docs/milestone-credential-store.md; then
    echo "FAIL: documentation exposed synthetic credential"
    exit 1
  fi
  if qemu-arm -L /opt/tc/arm-linux-musleabi-cross/arm-linux-musleabi \
       /io/testapp/crossnook-credentials-test --smoke "$ROOT/allowed" unexpected \
       > "$ROOT/invalid" 2>&1; then
    echo "FAIL: credential arguments accepted"
    exit 1
  fi
  if grep -F -e "crossnook-synthetic-account-only" \
     -e "crossnook-synthetic-nonreal-key-only" \
     -e "crossnook-synthetic-alternate-only" "$ROOT/invalid"; then
    echo "FAIL: invalid-CLI diagnostic exposed synthetic credential"
    exit 1
  fi
  cat "$ROOT/output"
  cat "$ROOT/redaction-ok"
  cat "$ROOT/redaction-negative"
  echo "CREDENTIAL STORE HOST VALIDATION OK"
'
sha256sum "$REPO/testapp/crossnook-credentials-test"
