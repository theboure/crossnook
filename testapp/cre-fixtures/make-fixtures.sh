#!/usr/bin/env bash
# Regenerate the CREngine EPUB fixture deterministically.
# Produces test.epub and prints its sha256 checksum.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
PY=${PYTHON:-python}
if ! command -v "$PY" >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1; then
  PY=python3
fi
"$PY" "$DIR/make_test_epub.py" "$DIR/test.epub"
echo -n "cre-fixtures/test.epub sha256: "
sha256sum "$DIR/test.epub" | awk '{print $1}'