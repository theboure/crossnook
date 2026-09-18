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

int cn_reader_render(cn_reader *r, void *rgb565, int w, int h)
{
    if (!r || !r->doc)
        return -1;
    if (!rgb565 || w != BUF_W || h != BUF_H)
        return -1;
    return render_into(r, rgb565);
}