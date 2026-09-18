#!/usr/bin/env bash
# Regenerate the Library -> Reader integration fixtures deterministically.
# valid.epub / valid2.epub are copies of the committed CREngine fixture
# (test.epub); broken.epub / book.fb2 / notes.txt are hand-written.
# Prints sha256 checksums at the end.
set -euo pipefail
DIR=$(cd "$(dirname "$0")" && pwd)
CRE=$DIR/../cre-fixtures/test.epub

# broken.epub: a zip archive that is NOT a valid EPUB (only a mimetype
# member, no container.xml / content). CREngine's LoadDocument must fail
# on it deterministically; plain text would be silently parsed as a
# 1-page txt document instead.
if command -v python >/dev/null 2>&1; then PY=python
elif command -v python3 >/dev/null 2>&1; then PY=python3
else PY=""
fi
if [ -n "$PY" ]; then
  $PY - "$DIR/broken.epub" <<'EOF'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_STORED)
z.writestr('mimetype', 'application/epub+zip')
z.close()
EOF
else
  echo "python not found; leaving existing broken.epub"
fi

cat > "$DIR/book.fb2" <<'EOF'
<?xml version="1.0" encoding="utf-8"?>
<FictionBook xmlns="http://www.gribuser.ru/xml/fictionbook/2.0">
  <description>
    <title-info>
      <book-title>book</book-title>
    </title-info>
  </description>
  <body>
    <section>
      <title><p>book</p></title>
      <p>Some text for the FB2 format fixture.</p>
    </section>
  </body>
</FictionBook>
EOF

cat > "$DIR/notes.txt" <<'EOF'
Plain text notes for the reader-fixtures set.
Line two, proving the TXT format is recognized.
EOF

cp "$CRE" "$DIR/valid.epub"
cp "$CRE" "$DIR/valid2.epub"

for f in broken.epub book.fb2 notes.txt valid.epub valid2.epub; do
  sha256sum "$DIR/$f"
done