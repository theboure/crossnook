#!/usr/bin/env python3
"""Generate a deterministic, redistributable test EPUB for the CREngine spike.

Multi-page, no DRM, EPUB2 (with toc.ncx) that koreader's CREngine opens with
no optional image codecs: headings (h1-h3), paragraphs, inline emphasis,
Latin and Cyrillic text, ordinary word wrapping. No images.

Determinism: every zip entry gets a fixed timestamp (1980-01-01), fixed
compression (STORED for mimetype, DEFLATE for the rest) and fixed content.
Running this twice produces byte-identical output (same Python/zlib), so the
book checksum recorded by build-cre.sh is a valid regression signal without
being overfit.

Usage: make_test_epub.py [output.epub]
"""
import os
import sys
import zipfile
import zlib
from datetime import datetime

FIXED_TS = (1980, 1, 1, 0, 0, 0)

XML_DECL = '<?xml version="1.0" encoding="utf-8"?>\n'

MIMETYPE = "application/epub+zip"

CONTAINER_XML = XML_DECL + (
    '<container version="1.0" '
    'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
    "  <rootfiles>\n"
    '    <rootfile full-path="OEBPS/content.opf" '
    'media-type="application/oebps-package+xml"/>\n'
    "  </rootfiles>\n"
    "</container>\n"
)


def xhtml_doc(body: str) -> str:
    return XML_DECL + (
        '<!DOCTYPE html>\n'
        '<html xmlns="http://www.w3.org/1999/xhtml" '
        'xml:lang="en" lang="en">\n'
        "<head>\n"
        '  <title>CrossNook Test Book</title>\n'
        "</head>\n"
        "<body>\n"
        + body
        + "</body>\n"
        "</html>\n"
    )


EN_WORDS = (
    "the quick brown fox jumps over the lazy dog and keeps on running "
    "through the wide open meadow under a pale sky while small birds "
    "circle above wondering where the summer went"
).split()

CYR_SENTENCES = (
    "Вот уже более ста пятидесяти лет читатели возвращаются к этому роману.",
    "Анна Каренина погружена в размышления о семье, долге и любви.",
    "Пушкин писал удивительно простые и ясные строки о времени и памяти.",
    "Осень наступила неожиданно, и листья пожелтели за одну неделю.",
    "Библиотека хранит тысячи томов, но каждая книга говорит по-своему.",
    "Читатель медленно переворачивает страницу и смотрит в окно.",
    "Тихий вечер опустился на город, и фонари зажглись один за другим.",
    "Слово — это маленький ключ к большой мысли обо всём на свете.",
)

# Deterministic paragraph factory: no randomness, just cycling word banks.
def en_paragraph(id: int, sentences: int) -> str:
    words = []
    for s in range(sentences):
        words.extend(EN_WORDS)
    text = " ".join(words)
    return f'<p id="p{id}">{text}</p>'

def cyr_paragraph(id: int, sentences: int) -> str:
    import itertools
    it = itertools.cycle(CYR_SENTENCES)
    parts = [next(it) for _ in range(sentences)]
    return f'<p id="p{id}">{" ".join(parts)}</p>'


def chapter(index: int) -> str:
    body = []
    if index == 1:
        body.append("<h1>CrossNook Test Book</h1>")
        body.append("<h2>Chapter One</h2>")
        body.append(
            "<p><strong>Purpose.</strong> This book exercises EPUB text "
            "rendering in CREngine: headings, paragraphs, inline emphasis, "
            "Latin and <em>Cyrillic</em> text, and plain word wrapping.</p>"
        )
        body.append("<h3>Section A</h3>")
        for p in range(1, 7):
            body.append(en_paragraph(p, 12))
        body.append("<h3>Section B</h3>")
        for p in range(7, 13):
            body.append(en_paragraph(p, 12))
    elif index == 2:
        body.append("<h2>Chapter Two</h2>")
        body.append(
            "<p>Эта глава проверяет отрисовку кириллицы и перенос строк на "
            "русском языке.</p>"
        )
        for p in range(1, 13):
            body.append(cyr_paragraph(p, 14))
    else:
        body.append("<h2>Chapter Three</h2>")
        body.append("<h3>Section A</h3>")
        for p in range(1, 11):
            body.append(en_paragraph(p, 14))
        body.append("<h3>Section B</h3>")
        for p in range(11, 23):
            body.append(cyr_paragraph(p, 12))
        body.append("<h3>Section C</h3>")
        for p in range(23, 31):
            body.append(en_paragraph(p, 14))
    return xhtml_doc("\n".join(body) + "\n")


CONTENT_OPF = XML_DECL + (
    '<package xmlns="http://www.idpf.org/2007/opf" version="2.0" '
    'unique-identifier="uid">\n'
    "  <metadata "
    'xmlns:dc="http://purl.org/dc/elements/1.1/" '
    'xmlns:opf="http://www.idpf.org/2007/opf">\n'
    "    <dc:title>CrossNook Test Book</dc:title>\n"
    "    <dc:creator>CrossNook Fixtures</dc:creator>\n"
    "    <dc:language>en</dc:language>\n"
    "    <dc:identifier id=\"uid\">urn:uuid:crossnook-test-0001</dc:identifier>\n"
    "  </metadata>\n"
    "  <manifest>\n"
    '    <item id="toc" href="toc.ncx" media-type="application/x-dtbncx+xml"/>\n'
    '    <item id="c1" href="chapter-1.xhtml" media-type="application/xhtml+xml"/>\n'
    '    <item id="c2" href="chapter-2.xhtml" media-type="application/xhtml+xml"/>\n'
    '    <item id="c3" href="chapter-3.xhtml" media-type="application/xhtml+xml"/>\n'
    "  </manifest>\n"
    '  <spine toc="toc">\n'
    '    <itemref idref="c1"/>\n'
    '    <itemref idref="c2"/>\n'
    '    <itemref idref="c3"/>\n'
    "  </spine>\n"
    "</package>\n"
)

TOC_NCX = XML_DECL + (
    '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">\n'
    "  <head>\n"
    "    <meta name=\"dtb:uid\" content=\"urn:uuid:crossnook-test-0001\"/>\n"
    "    <meta name=\"dtb:depth\" content=\"1\"/>\n"
    "  </head>\n"
    "  <docTitle><text>CrossNook Test Book</text></docTitle>\n"
    "  <navMap>\n"
    '    <navPoint id="nav1" playOrder="1">\n'
    '      <navLabel><text>Chapter One</text></navLabel>\n'
    '      <content src="chapter-1.xhtml"/>\n'
    "    </navPoint>\n"
    '    <navPoint id="nav2" playOrder="2">\n'
    '      <navLabel><text>Chapter Two</text></navLabel>\n'
    '      <content src="chapter-2.xhtml"/>\n'
    "    </navPoint>\n"
    '    <navPoint id="nav3" playOrder="3">\n'
    '      <navLabel><text>Chapter Three</text></navLabel>\n'
    '      <content src="chapter-3.xhtml"/>\n'
    "    </navPoint>\n"
    "  </navMap>\n"
    "</ncx>\n"
)


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else "test.epub"
    entries = [
        ("mimetype", MIMETYPE.encode(), zipfile.ZIP_STORED),
        ("META-INF/container.xml", CONTAINER_XML.encode(), zipfile.ZIP_DEFLATED),
        ("OEBPS/content.opf", CONTENT_OPF.encode(), zipfile.ZIP_DEFLATED),
        ("OEBPS/toc.ncx", TOC_NCX.encode(), zipfile.ZIP_DEFLATED),
        ("OEBPS/chapter-1.xhtml", chapter(1).encode(), zipfile.ZIP_DEFLATED),
        ("OEBPS/chapter-2.xhtml", chapter(2).encode(), zipfile.ZIP_DEFLATED),
        ("OEBPS/chapter-3.xhtml", chapter(3).encode(), zipfile.ZIP_DEFLATED),
    ]
    with zipfile.ZipFile(out, "w") as zf:
        for name, data, method in entries:
            info = zipfile.ZipInfo(name, date_time=FIXED_TS)
            info.compress_type = method
            info.external_attr = 0o644 << 16
            zf.writestr(info, data)


if __name__ == "__main__":
    main()