/*
 * reader.cpp — reusable EPUB reader layer (CREngine implementation).
 *
 * See reader.h for the C API contract. This file is the ONLY place that
 * touches CREngine for the book reader; it reproduces the hardware-
 * validated settings from the CREngine spike:
 *
 *   InitFontManager(""); fontMan->RegisterFont(font);
 *   LVDocView(16); setStyleSheet(cre_css, true); setViewMode(DVM_PAGES,1);
 *   Resize(600,800); LoadDocument(path);
 *   first Draw() performs layout; then getPageCount() is valid.
 *   LVColorDrawBuf(600,800,target,16) draws RGB565 straight into the
 *   caller buffer (row stride 1200).
 *
 * CREngine never opens /dev/graphics/fb0; it only renders into memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

extern "C" {
#include "reader/reader.h"
}

#include "cre_css.h"
#include "lvdrawbuf.h"
#include "lvfntman.h"
#include "lvstring.h"
#include "lvdocview.h"

#define BUF_W CN_READER_W
#define BUF_H CN_READER_H
#define BUF_BYTES (BUF_W * BUF_H * 2)

static bool g_font_ready = false;    /* fontMan singleton initialized  */
static lString8 g_font_path;         /* last font registered by us     */

struct cn_reader {
    LVDocView *doc;
    cn_reader_config cfg;
    int  pages;
    int  page;
    int  last_diag_page;   /* -1 = no page diag yet */
    lUInt8 *scratch;       /* first-Draw layout priming buffer */
};

/* ---- internal helpers ---------------------------------------------- */

/* koreader's RegisterFont returns false when the exact filename was
 * already registered by this process (it is a "new face created" signal,
 * not an error). Track what we have registered so that several cn_reader
 * instances sharing one font can coexist. */
static bool ensure_font(const char *font)
{
    if (!g_font_ready) {
        InitFontManager(lString8(""));
        g_font_ready = true;
    }
    if (!font || !font[0])
        return true;   /* engine default font search happens at open */
    if (g_font_path == lString8(font))
        return true;   /* already registered by this process */
    if (!fontMan->RegisterFont(lString8(font)))
        return false;
    g_font_path = font;
    return true;
}

static int render_into(cn_reader *r, void *rgb565)
{
    memset(rgb565, 0xFF, BUF_BYTES);
    LVColorDrawBuf buf(BUF_W, BUF_H, (lUInt8 *)rgb565, 16);
    r->doc->Draw(buf);
    return 0;
}

static void emit_page(cn_reader *r)
{
    if (r->page == r->last_diag_page)
        return;
    fprintf(stderr, "READER page=%d\n", r->page);
    r->last_diag_page = r->page;
}

static int position_location_length(const char *location, size_t *length)
{
    size_t n;
    if (!location || location[0] != '/')
        return -1;
    for (n = 0; n <= CN_READER_POSITION_MAX_BYTES; ++n) {
        if (location[n] == '\0') {
            if (n == 0)
                return -1;
            *length = n;
            return 0;
        }
    }
    return -1;
}

static int position_utf8_valid(const char *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        unsigned char c = (unsigned char)text[i++];
        int continuation;
        unsigned char min_second = 0x80;
        unsigned char max_second = 0xbf;

        if (c <= 0x7f)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            continuation = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            continuation = 2;
            if (c == 0xe0)
                min_second = 0xa0;
            else if (c == 0xed)
                max_second = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            continuation = 3;
            if (c == 0xf0)
                min_second = 0x90;
            else if (c == 0xf4)
                max_second = 0x8f;
        } else {
            return -1;
        }
        if (i + (size_t)continuation > length)
            return -1;
        if ((unsigned char)text[i] < min_second ||
            (unsigned char)text[i] > max_second)
            return -1;
        ++i;
        while (--continuation > 0) {
            if ((unsigned char)text[i] < 0x80 ||
                (unsigned char)text[i] > 0xbf)
                return -1;
            ++i;
        }
    }
    return 0;
}

static int position_decimal_valid(const char *text, size_t start, size_t end)
{
    int value = 0;
    size_t i;
    if (start == end)
        return -1;
    for (i = start; i < end; ++i) {
        int digit;
        if (text[i] < '0' || text[i] > '9')
            return -1;
        digit = text[i] - '0';
        if (value > (INT_MAX - digit) / 10)
            return -1;
        value = value * 10 + digit;
    }
    return 0;
}

/* CREngine's XPointer parser converts bracket, node, and point indexes to
 * signed int. Validate these fields before handing untrusted serialized data
 * to it so malformed values cannot reach its integer conversion. */
static int position_indexes_valid(const char *text, size_t length)
{
    size_t i;
    for (i = 0; i < length; ++i) {
        size_t start;
        size_t end;
        if (text[i] == '[') {
            start = ++i;
            while (i < length && text[i] != ']')
                ++i;
            if (i == length ||
                position_decimal_valid(text, start, i) != 0)
                return -1;
        } else if ((text[i] == '/' || text[i] == '.') &&
                   i + 1 < length && text[i + 1] >= '0' &&
                   text[i + 1] <= '9') {
            start = ++i;
            while (i < length && text[i] >= '0' && text[i] <= '9')
                ++i;
            end = i;
            if (position_decimal_valid(text, start, end) != 0)
                return -1;
            --i;
        }
    }
    return 0;
}

/* Resolve only canonical tokens generated by this layer. Re-serializing the
 * parsed XPointer catches malformed or partially accepted paths before any
 * reader state is changed. */
static int resolve_position(cn_reader *r, const cn_reader_position *position,
                            ldomXPointer *resolved)
{
    size_t length;
    if (!r || !r->doc || !position ||
        position_location_length(position->location, &length) != 0)
        return -1;
    if (position_utf8_valid(position->location, length) != 0 ||
        position_indexes_valid(position->location, length) != 0)
        return -1;

    lString32 encoded = Utf8ToUnicode(position->location, (int)length);
    ldomXPointer pointer = r->doc->getDocument()->createXPointer(encoded);
    if (pointer.isNull())
        return -1;

    lString8 canonical = UnicodeToUtf8(pointer.toString());
    if (strcmp(canonical.c_str(), position->location) != 0)
        return -1;
    if (pointer.toPoint().y < 0)
        return -1;

    *resolved = pointer;
    return 0;
}

/* Apply the layout-affecting options (ignored when at engine defaults).
 * Only the options the pinned CREngine exposes are applied: font size,
 * interline space percentage and page margins. */
static void apply_layout(cn_reader *r)
{
    if (r->cfg.font_size > 0)
        r->doc->setFontSize(r->cfg.font_size);
    if (r->cfg.line_spacing_pct > 0 && r->cfg.line_spacing_pct != 100)
        r->doc->setDefaultInterlineSpace(r->cfg.line_spacing_pct);
    if (r->cfg.margin_px > 0)
        r->doc->setPageMargins(lvRect(r->cfg.margin_px, r->cfg.margin_px,
                                      r->cfg.margin_px, r->cfg.margin_px));
}

/* Force a controlled re-layout: apply options, resize, prime layout with a
 * first Draw, then refresh page count and clamp the current page. */
static void relayout(cn_reader *r)
{
    apply_layout(r);
    r->doc->Resize(BUF_W, BUF_H);
    r->doc->goToPage(0);
    (void)render_into(r, r->scratch);
    r->pages = r->doc->getPageCount();
    if (r->pages < 1)
        r->pages = 1;
    r->page = 0;
    fprintf(stderr, "READER pages=%d\n", r->pages);
    emit_page(r);
}

/* ---- public API ---------------------------------------------------- */

cn_reader *cn_reader_new(const cn_reader_config *cfg)
{
    cn_reader *r = (cn_reader *)calloc(1, sizeof *r);
    if (!r)
        return NULL;

    if (cfg)
        r->cfg = *cfg;

    const char *font = r->cfg.font_path;
    if (!font) {
        static const char *cand[] = { "/tmp/test-font.ttf",
                                      "/opt/test-font.ttf" };
        for (int i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); ++i) {
            FILE *f = fopen(cand[i], "rb");
            if (f) {
                fclose(f);
                font = cand[i];
                r->cfg.font_path = font;
                break;
            }
        }
    }
    if (!ensure_font(font)) {
        fprintf(stderr, "READER ERROR: font register failed (%s)\n",
                font ? font : "<no font found>");
        free(r);
        return NULL;
    }

    r->scratch = (lUInt8 *)malloc(BUF_BYTES);
    if (!r->scratch) {
        free(r);
        return NULL;
    }
    r->last_diag_page = -1;
    return r;
}

void cn_reader_free(cn_reader *r)
{
    if (!r)
        return;
    delete r->doc;   /* NULL-safe */
    free(r->scratch);
    free(r);
}

void cn_reader_position_init(cn_reader_position *position)
{
    if (!position)
        return;
    position->location = NULL;
    position->progress_10000 = -1;
}

void cn_reader_position_clear(cn_reader_position *position)
{
    if (!position)
        return;
    free(position->location);
    position->location = NULL;
    position->progress_10000 = -1;
}

int cn_reader_position_copy(cn_reader_position *dst,
                            const cn_reader_position *src)
{
    cn_reader_position copy;
    size_t length;
    if (!dst || !src)
        return -1;
    if (dst == src)
        return 0;

    cn_reader_position_init(&copy);
    copy.progress_10000 = src->progress_10000;
    if (src->location) {
        if (position_location_length(src->location, &length) != 0)
            return -1;
        copy.location = (char *)malloc(length + 1);
        if (!copy.location)
            return -1;
        memcpy(copy.location, src->location, length + 1);
    }

    cn_reader_position_clear(dst);
    *dst = copy;
    return 0;
}

int cn_reader_apply_config(cn_reader *r, const cn_reader_config *cfg)
{
    if (!r)
        return -1;
    if (cfg) {
        const char *old = r->cfg.font_path;
        r->cfg = *cfg;
        if ((old == NULL) != (r->cfg.font_path == NULL) ||
            (old && r->cfg.font_path &&
             strcmp(old, r->cfg.font_path) != 0)) {
            /* new font: register it before the next document opens */
            if (!ensure_font(r->cfg.font_path)) {
                fprintf(stderr, "READER ERROR: font register failed (%s)\n",
                        r->cfg.font_path);
                return -1;
            }
        }
    }
    if (r->doc)
        relayout(r);
    return 0;
}

int cn_reader_open(cn_reader *r, const char *path)
{
    if (!r || !path)
        return -1;
    cn_reader_close(r);

    r->doc = new LVDocView(16);
    r->doc->setStyleSheet(lString8(cre_css), true);
    r->doc->setViewMode(DVM_PAGES, 1);
    r->doc->Resize(BUF_W, BUF_H);
    apply_layout(r);

    if (!r->doc->LoadDocument(path)) {
        fprintf(stderr, "READER ERROR: open failed: %s\n", path);
        delete r->doc;
        r->doc = NULL;
        return -1;
    }

    fprintf(stderr, "READER open path=%s\n", path);
    relayout(r);
    return 0;
}

void cn_reader_close(cn_reader *r)
{
    if (!r || !r->doc)
        return;
    fprintf(stderr, "READER close\n");
    delete r->doc;
    r->doc = NULL;
    r->pages = 0;
    r->page = 0;
    r->last_diag_page = -1;
}

int cn_reader_is_open(const cn_reader *r)
{
    return r && r->doc ? 1 : 0;
}

int cn_reader_pages(const cn_reader *r)
{
    return r && r->doc ? r->pages : 0;
}

int cn_reader_page(const cn_reader *r)
{
    return r && r->doc ? r->page : 0;
}

int cn_reader_go(cn_reader *r, int page)
{
    if (!r || !r->doc)
        return 0;
    if (page < 0)
        page = 0;
    if (page > r->pages - 1)
        page = r->pages - 1;
    if (page != r->page) {
        r->doc->goToPage(page);
        r->page = page;
        emit_page(r);
    }
    return r->page;
}

int cn_reader_next(cn_reader *r)
{
    if (!r || !r->doc)
        return 0;
    return cn_reader_go(r, r->page + 1);   /* clamps at last page */
}

int cn_reader_prev(cn_reader *r)
{
    if (!r || !r->doc)
        return 0;
    return cn_reader_go(r, r->page - 1);   /* clamps at page 0 */
}

int cn_reader_get_position(cn_reader *r, cn_reader_position *position)
{
    cn_reader_position captured;
    ldomXPointer pointer;
    lString8 location;
    lvPoint point;
    int full_height;
    size_t length;

    if (!r || !r->doc || !position)
        return -1;
    pointer = r->doc->getBookmark(true);
    if (pointer.isNull())
        return -1;
    location = UnicodeToUtf8(pointer.toString());
    if (position_location_length(location.c_str(), &length) != 0 ||
        position_utf8_valid(location.c_str(), length) != 0 ||
        position_indexes_valid(location.c_str(), length) != 0)
        return -1;

    cn_reader_position_init(&captured);
    captured.location = (char *)malloc(length + 1);
    if (!captured.location)
        return -1;
    memcpy(captured.location, location.c_str(), length + 1);

    point = pointer.toPoint();
    full_height = r->doc->GetFullHeight();
    if (full_height > 0 && point.y >= 0) {
        captured.progress_10000 =
            (int)((lInt64)point.y * 10000 / full_height);
        if (captured.progress_10000 < 0)
            captured.progress_10000 = 0;
        if (captured.progress_10000 > 10000)
            captured.progress_10000 = 10000;
    }

    cn_reader_position_clear(position);
    *position = captured;
    return 0;
}

int cn_reader_goto_position(cn_reader *r,
                            const cn_reader_position *position)
{
    ldomXPointer pointer;
    int page;
    if (resolve_position(r, position, &pointer) != 0)
        return -1;

    page = r->doc->getBookmarkPage(pointer);
    if (page < 0 || page >= r->pages)
        return -1;

    r->doc->goToBookmark(pointer);
    r->page = r->doc->getCurPage();
    emit_page(r);
    return 0;
}

int cn_reader_render(cn_reader *r, void *rgb565, int w, int h)
{
    if (!r || !r->doc)
        return -1;
    if (!rgb565 || w != BUF_W || h != BUF_H)
        return -1;
    return render_into(r, rgb565);
}
