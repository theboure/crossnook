#!/usr/bin/env bash
# Fetch the exact BearSSL source artifact consumed by toolchain/Dockerfile.
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
DEST="$REPO/work/bearssl"
NAME="bearssl_0.6+dfsg.1.orig.tar.xz"
URL="https://deb.debian.org/debian/pool/main/b/bearssl/$NAME"
SHA256="102a625bd8f6155260867a50c508df8d8c3f796985dd744df1a68e5cd1a79004"

mkdir -p "$DEST"
if test -f "$DEST/$NAME" &&
   printf '%s  %s\n' "$SHA256" "$DEST/$NAME" | sha256sum -c - >/dev/null; then
  echo "BearSSL source pin already present"
  exit 0
fi
rm -f "$DEST/$NAME.tmp"
curl --fail --location --retry 3 --output "$DEST/$NAME.tmp" "$URL"
printf '%s  %s\n' "$SHA256" "$DEST/$NAME.tmp" | sha256sum -c -
mv "$DEST/$NAME.tmp" "$DEST/$NAME"
echo "BearSSL source pin fetched and verified"
