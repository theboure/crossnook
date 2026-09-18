#!/usr/bin/env python3
"""Generate a deterministic EPUB with a DOM unlike the main test book."""
import sys
import zipfile


FIXED_TS = (1980, 1, 1, 0, 0, 0)
XML = '<?xml version="1.0" encoding="utf-8"?>\n'

ENTRIES = {
    "mimetype": "application/epub+zip",
    "META-INF/container.xml": XML +
        '<container version="1.0" '
        'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles></container>',
    "OEBPS/content.opf": XML +
        '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" '
        'unique-identifier="uid"><metadata '
        'xmlns:dc="http://purl.org/dc/elements/1.1/">'
        '<dc:title>Foreign Position Fixture</dc:title>'
        '<dc:language>en</dc:language>'
        '<dc:identifier id="uid">urn:crossnook:foreign</dc:identifier>'
        '</metadata><manifest><item id="only" href="only.xhtml" '
        'media-type="application/xhtml+xml"/></manifest>'
        '<spine><itemref idref="only"/></spine></package>',
    "OEBPS/only.xhtml": XML +
        '<html xmlns="http://www.w3.org/1999/xhtml"><head>'
        '<title>Foreign</title></head><body><h1>Foreign document with no '
        'paragraph elements</h1></body></html>',
}


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make-foreign.py <output.epub>")
    with zipfile.ZipFile(sys.argv[1], "w") as archive:
        for name, text in ENTRIES.items():
            info = zipfile.ZipInfo(name, date_time=FIXED_TS)
            info.create_system = 3
            info.external_attr = 0o644 << 16
            info.compress_type = zipfile.ZIP_STORED
            archive.writestr(info, text.encode("utf-8"))


if __name__ == "__main__":
    main()
