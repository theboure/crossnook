/*
 * crossnook-cre-test.cpp — CREngine EPUB rendering spike testapp.
 *
 * Proves CrossNook can use CREngine (koreader/crengine @ b05cf007) to open a
 * plain EPUB and render readable pages as RGB565 (16bpp) bitmaps for the
 * Nook Simple Touch's 600x800 framebuffer. Renders straight into the existing
 * RGB565 canvas (or a dumb host buffer); CREngine never touches fb0 — only
 * src/platform/nook/display.c does (validated structurally by build-cre.sh).
 *
 * Usage:
 *   crossnook-cre-test <epub>                     device mode (real Nook)
 *   crossnook-cre-test --smoke   <epub>           host smoke assertions
 *   crossnook-cre-test --dump DIR <epub>          host page dumps + manifest
 *
 * Always registers a font first. Font = --font PATH, else /tmp/test-font.ttf
 * then /opt/test-font.ttf (the last two are the on-device install paths).
 *
 * Failure contract: any failure prints "CRE ERROR: ..." to stderr and exits
 * non-zero; the screen is never left blank in device mode.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* crengine headers */
#include "lvdocview.h"
#include "lvdrawbuf.h"
#include "lvfntman.h"
#include "lvstring.h"

#include "cre_css.h"

extern "C" {
#include "graphics/canvas.h"
#include "platform/nook/display.h"
#include "platform/nook/input.h"
}

#define BUF_W 600
#define BUF_H 800
#define BUF_BYTES (BUF_W * BUF_H * 2)

enum mode { MODE_DEVICE, MODE_SMOKE, MODE_DUMP };

static const char *g_font = NULL;
static const char *g_epub = NULL;

static int find_default_font(void)
{
    static const char *cand[] = { "/tmp/test-font.ttf", "/opt/test-font.ttf" };
    FILE *f;
    for (int i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); ++i) {
        f = fopen(cand[i], "rb");
        if (f) {
            fclose(f);
            g_font = cand[i];
            return 0;
        }
    }
    return -1;
}

static int parse_args(int argc, char **argv, enum mode *mode, const char **dump_dir)
{
    *dump_dir = NULL;
    *mode = MODE_DEVICE;
    int i = 1;
    for (; i < argc; ++i) {
        if (strcmp(argv[i], "--font") == 0 && i + 1 < argc) {
            g_font = argv[++i];
        } else if (strcmp(argv[i], "--smoke") == 0) {
            *mode = MODE_SMOKE;
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            *mode = MODE_DUMP;
            *dump_dir = argv[++i];
        } else if (argv[i][0] != '-') {
            g_epub = argv[i];
        } else {
            return -1;
        }
    }
    return g_epub ? 0 : -1;
}

/* FNV-1a 32-bit over the target buffer. */
static unsigned checksum_bytes(const lUInt8 *p, size_t n)
{
    unsigned h = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned long count_ink(const lUInt8 *p, size_t n)
{
    const lUInt16 *px = (const lUInt16 *)p;
    size_t np = n / 2;
    unsigned long ink = 0;
    for (size_t i = 0; i < np; ++i)
        if (px[i] != 0xFFFF)
            ++ink;
    return ink;
}

/* Renders current CRE page into target (memset to white first). */
static unsigned render_pixel_checksum(LVDocView *doc, lUInt8 *target)
{
    memset(target, 0xFF, BUF_BYTES);
    LVColorDrawBuf buf(BUF_W, BUF_H, target, 16);
    doc->Draw(buf);
    return checksum_bytes(target, BUF_BYTES);
}

/* ---- host smoke -------------------------------------------------- */

static int run_smoke(LVDocView *doc)
{
    int fail = 0;

    const size_t GUARD = 64;
    lUInt8 *base = new lUInt8[GUARD + BUF_BYTES + GUARD];
    memset(base, 0x5A, GUARD + BUF_BYTES + GUARD);
    lUInt8 *target = base + GUARD;

    /* First Draw performs layout; only afterwards is the page count valid. */
    doc->goToPage(0);
    unsigned m0 = render_pixel_checksum(doc, target);
    unsigned long i0 = count_ink(target, BUF_BYTES);

    const int pages = doc->getPageCount();
    printf("document loaded: %d pages\n", pages);
    if (pages < 2) {
        fprintf(stderr, "CRE ERROR: book has %d pages (< 2)\n", pages);
        delete[] base;
        return 1;
    }

    if (doc->GetWidth() != BUF_W || doc->GetHeight() != BUF_H) {
        fprintf(stderr, "CRE ERROR: unexpected page dims\n");
        delete[] base;
        return 1;
    }
    printf("layout size=%dx%d\n", doc->GetWidth(), doc->GetHeight());

    printf("page=0 ink=%lu chk=%08x\n", i0, m0);
    if (i0 == 0) {
        fprintf(stderr, "CRE ERROR: page 0 has no ink\n");
        fail = 1;
    }

    unsigned m1 = 0;
    unsigned long i1 = 0;
    doc->goToPage(1);
    unsigned c = render_pixel_checksum(doc, target);
    i1 = count_ink(target, BUF_BYTES);
    m1 = c;
    printf("page=1 ink=%lu chk=%08x\n", i1, m1);
    if (i1 == 0) {
        fprintf(stderr, "CRE ERROR: page 1 has no ink\n");
        fail = 1;
    }
    if (m1 == m0) {
        fprintf(stderr, "CRE ERROR: page 1 identical to page 0\n");
        fail = 1;
    }

    doc->goToPage(0);
    unsigned m_back = render_pixel_checksum(doc, target);
    printf("page=0 back chk=%08x\n", m_back);
    if (m_back != m0) {
        fprintf(stderr, "CRE ERROR: page 0 after PREV differs from first render\n");
        fail = 1;
    }

    for (size_t g = 0; g < GUARD; ++g) {
        if (base[g] != 0x5A || base[GUARD + BUF_BYTES + g] != 0x5A) {
            fprintf(stderr, "CRE ERROR: canary corrupted (OOB write)\n");
            fail = 1;
            break;
        }
    }

    delete[] base;

    LVColorDrawBuf probe(BUF_W, BUF_H, (lUInt8 *)target, 16);
    if (probe.GetWidth() != BUF_W || probe.GetHeight() != BUF_H ||
        probe.GetRowSize() != BUF_W * 2) {
        fprintf(stderr, "CRE ERROR: buffer geometry mismatch\n");
        fail = 1;
    }

    printf(fail ? "CRE SMOKE FAIL\n" : "CRE SMOKE OK\n");
    return fail ? 1 : 0;
}

/* ---- host dump --------------------------------------------------- */

static int run_dump(LVDocView *doc, const char *dir)
{
    char path[1024];

    lUInt8 *target = new lUInt8[BUF_BYTES];

    /* First Draw performs layout; only afterwards is the page count valid. */
    doc->goToPage(0);
    render_pixel_checksum(doc, target);

    const int pages = doc->getPageCount();
    const int w = doc->GetWidth();
    const int h = doc->GetHeight();
    const int rsz = BUF_W * 2;
    snprintf(path, sizeof(path), "%s/dump.json", dir);
    FILE *jf = fopen(path, "w");
    if (!jf) {
        fprintf(stderr, "CRE ERROR: cannot write %s\n", path);
        delete[] target;
        return -1;
    }
    fprintf(jf, "{\"page_count\":%d,\"width\":%d,\"height\":%d,"
                "\"row_size\":%d,\"pages\":[",
            pages, w, h, rsz);

    int dumped = 0;
    for (int p = 0; p < pages && p < 16; ++p) {
        doc->goToPage(p);
        unsigned chk = render_pixel_checksum(doc, target);
        char fn[64];
        snprintf(fn, sizeof(fn), "page-%03d.bin", p);
        snprintf(path, sizeof(path), "%s/%s", dir, fn);
        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "CRE ERROR: cannot write %s\n", path);
            delete[] target;
            fclose(jf);
            return -1;
        }
        fwrite(target, 1, BUF_BYTES, f);
        fclose(f);
        unsigned long ink = count_ink(target, BUF_BYTES);
        fprintf(jf, "%s{\"page\":%d,\"file\":\"%s\",\"checksum\":\"%08x\","
                    "\"ink\":%lu}",
                dumped ? "," : "", p, fn, chk, ink);
        ++dumped;
        printf("page=%d ink=%lu chk=%08x -> %s\n", p, ink, chk, fn);
    }
    fprintf(jf, "]}");
    fclose(jf);
    delete[] target;
    printf("CRE DUMP OK (%d pages)\n", dumped);
    return dumped;
}

/* ---- device mode -------------------------------------------------- */

static int run_device(LVDocView *doc)
{
    cn_display *disp = cn_display_open();
    if (!disp) {
        fprintf(stderr, "CRE ERROR: display open failed\n");
        return 1;
    }
    cn_canvas *c = cn_canvas_create(BUF_W, BUF_H);
    cn_input *in = cn_input_open();
    if (!c || !in) {
        fprintf(stderr, "CRE ERROR: alloc/input open failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        return 1;
    }
    printf("inputs=%d\n", cn_input_devices(in));

    int page = 0, want = 0;
    cn_input_ev ev;
    int rc;
    int power_pressed = 0;

    for (;;) {
        if (want != page) {
            page = want;
            doc->goToPage(page);
        }
        render_pixel_checksum(doc, (lUInt8 *)cn_canvas_pixels(c));
        if (cn_display_flush(disp, cn_canvas_pixels(c)) != CN_FB_BYTES) {
            fprintf(stderr, "CRE ERROR: fb write failed\n");
            cn_input_close(in);
            cn_canvas_free(c);
            cn_display_close(disp);
            return 1;
        }
        printf("page=%d\n", page);

        rc = cn_input_poll(in, &ev, -1);
        if (rc < 0)
            break;
        if (rc == 0)
            continue;

        if (ev.type == CN_INPUT_PAGE_NEXT) {
            if (page + 1 < (int)doc->getPageCount())
                want = page + 1;
        } else if (ev.type == CN_INPUT_PAGE_PREV) {
            if (page > 0)
                want = page - 1;
        } else if (ev.type == CN_INPUT_BACK || ev.type == CN_INPUT_HOME) {
            printf("CRE exit (back/home)\n");
            break;
        } else if (ev.type == CN_INPUT_POWER_DOWN) {
            power_pressed = 1;
        } else if (ev.type == CN_INPUT_POWER_UP) {
            if (power_pressed) {
                printf("CRE exit (power)\n");
                break;
            }
        }
    }

    cn_input_close(in);
    cn_canvas_free(c);
    cn_display_close(disp);
    return 0;
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv)
{
    enum mode m;
    const char *dump_dir = NULL;
    if (parse_args(argc, argv, &m, &dump_dir) != 0) {
        fprintf(stderr, "CRE ERROR: usage: crossnook-cre-test "
                        "[--font PATH] [--smoke|--dump DIR] <epub>\n");
        return 1;
    }

    if (!g_font && find_default_font() != 0) {
        fprintf(stderr, "CRE ERROR: no font found (pass --font PATH)\n");
        return 1;
    }

    InitFontManager(lString8(""));
    if (!fontMan->RegisterFont(lString8(g_font))) {
        fprintf(stderr, "CRE ERROR: font register failed (%s)\n", g_font);
        return 1;
    }
    printf("CRE open OK\n");

    LVDocView *doc = new LVDocView(16);
    doc->setStyleSheet(lString8(cre_css), true);
    doc->setViewMode(DVM_PAGES, 1);
    doc->Resize(BUF_W, BUF_H);

    if (!doc->LoadDocument(g_epub)) {
        fprintf(stderr, "CRE ERROR: LoadDocument failed (%s)\n", g_epub);
        delete doc;
        return 1;
    }

    int rc = 0;
    if (m == MODE_SMOKE)
        rc = run_smoke(doc);
    else if (m == MODE_DUMP)
        rc = run_dump(doc, dump_dir) < 0 ? 1 : 0;
    else
        rc = run_device(doc);

    delete doc;
    return rc;
}