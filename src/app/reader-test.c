/*
 * reader-test.c — CrossNook Library -> Reader Integration milestone
 * executable.
 *
 * Composes the extracted modules + the EPUB reader:
 *   cn_display (sole fb0 owner), cn_canvas, cn_text, cn_input, cn_library,
 *   cn_ui (HOME / READER_TEST / LIBRARY / SELECTED_BOOK / READER),
 *   cn_reader (CREngine, links in src/reader/reader.cpp).
 *
 * EPUB activation opens the real reader at page 0; FB2/TXT activation and a
 * failing EPUB open land on the deterministic SELECTED_BOOK diagnostic.
 *
 * Modes:
 *   crossnook-reader-test <font.ttf> <books-dir> <state-dir>  device mode
 *   crossnook-reader-test --smoke <font> <dir>                host checks
 *   crossnook-reader-test --dump <font> <dir> <outdir>        host frames
 *
 * Host modes never open /dev/graphics/fb0 or /dev/input/event*.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "graphics/canvas.h"
#include "graphics/text.h"
#include "library/library.h"
#include "platform/nook/display.h"
#include "platform/nook/input.h"
#include "progress/book_identity.h"
#include "progress/progress_store.h"
#include "ui/ui.h"

/* ---- host helpers -------------------------------------------------- */

static int dump_pixels(const char *path, const cn_canvas *c)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const unsigned char *p =
        (const unsigned char *)cn_canvas_pixels((cn_canvas *)c);
    size_t len = (size_t)cn_canvas_width(c) * cn_canvas_height(c) * 2;

    if (fd < 0) {
        fprintf(stderr, "dump: open %s: %s\n", path, strerror(errno));
        return 1;
    }
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "dump: write %s: %s\n", path, strerror(errno));
            close(fd);
            return 1;
        }
        if (n == 0) {
            fprintf(stderr, "dump: short write %s\n", path);
            close(fd);
            return 1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}

/* FNV-1a 32-bit over the framebuffer bytes (matches the python validators). */
static uint32_t checksum_fb(const cn_canvas *c)
{
    const unsigned char *p =
        (const unsigned char *)cn_canvas_pixels((cn_canvas *)c);
    size_t n = (size_t)cn_canvas_width(c) * cn_canvas_height(c) * 2;
    size_t i;
    uint32_t h = 2166136261u;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static unsigned long ink_count(const cn_canvas *c)
{
    const uint16_t *px = cn_canvas_pixels((cn_canvas *)c);
    int n = cn_canvas_width(c) * cn_canvas_height(c);
    int i;
    unsigned long ink = 0;
    for (i = 0; i < n; i++)
        if (px[i] != CN_COLOR_WHITE)
            ink++;
    return ink;
}

static void set_touch(cn_input_ev *ev, cn_input_event type, int x, int y)
{
    memset(ev, 0, sizeof *ev);
    ev->type = type;
    ev->x = x;
    ev->y = y;
}

static void tap(cn_ui *ui, int x, int y)
{
    cn_input_ev ev;
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, x, y);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_TOUCH_UP, x, y);
    cn_ui_handle(ui, &ev);
}

/* Tap the library row of `idx` (valid while that row is on screen). */
static void tap_row(cn_ui *ui, int idx)
{
    int y = CN_UI_LIB_ROW_TOP + idx * CN_UI_LIB_ROW_H + 10;
    tap(ui, 200, y);
}

static void key(cn_ui *ui, cn_input_event type)
{
    cn_input_ev ev;
    set_touch(&ev, type, 0, 0);
    cn_ui_handle(ui, &ev);
}

static int find_index(const cn_library *lib, const char *title)
{
    int i;
    for (i = 0; i < cn_library_count(lib); i++) {
        const cn_book *b = cn_library_get(lib, i);
        if (strcmp(b->title, title) == 0)
            return i;
    }
    return -1;
}

/* Activate `idx` from the library viewport: tap to select, tap to open. */
static void activate_index(cn_ui *ui, int idx)
{
    tap_row(ui, idx);
    tap_row(ui, idx);
}

/* ---- direct reader API: OOB guards + invalid open + reconfig --------- */

static int test_reader_direct(const char *font, const char *valid,
                              const char *broken)
{
    cn_reader_config cfg, cfg2;
    cn_reader *r;
    const size_t GUARD = 64;
    const size_t BUF = (size_t)CN_READER_W * CN_READER_H * 2;
    uint8_t *base = NULL;
    uint8_t *target;
    size_t g;
    int fail = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.font_path = font;
    r = cn_reader_new(&cfg);
    if (!r) {
        fprintf(stderr, "DIRECT: reader new failed\n");
        return 1;
    }

    /* invalid epub: open fails, no document stays open, rendering on the
     * closed reader is refused (target never written). */
    if (cn_reader_open(r, broken) == 0) {
        printf("SMOKE invalid epub opened FAIL\n");
        fail = 1;
    }
    if (cn_reader_is_open(r)) {
        printf("SMOKE reader open after failed load FAIL\n");
        fail = 1;
    }
    {
        uint8_t probe[64];
        memset(probe, 0xAA, sizeof probe);
        if (cn_reader_render(r, probe, CN_READER_W, CN_READER_H) != -1) {
            printf("SMOKE render on closed reader FAIL\n");
            fail = 1;
        }
        if (probe[0] != 0xAA) {
            printf("SMOKE closed-reader render wrote target FAIL\n");
            fail = 1;
        }
    }

    if (cn_reader_open(r, valid) != 0) {
        printf("SMOKE valid epub open FAIL\n");
        fail = 1;
    }

    /* NULL buffer and wrong dimensions must be refused with a document
     * open (no OOB, no silent geometry change). */
    if (cn_reader_render(r, NULL, CN_READER_W, CN_READER_H) != -1) {
        printf("SMOKE NULL target render FAIL\n");
        fail = 1;
    }
    if (cn_reader_render(r, (void *)&cfg, 599, 800) != -1) {
        printf("SMOKE wrong-width render FAIL\n");
        fail = 1;
    }

    /* canary-guarded render proves writes stay inside the buffer. */
    base = (uint8_t *)malloc(GUARD + BUF + GUARD);
    if (!base) {
        printf("SMOKE canary alloc FAIL\n");
        cn_reader_free(r);
        return 1;
    }
    memset(base, 0x5A, GUARD + BUF + GUARD);
    target = base + GUARD;
    if (cn_reader_render(r, target, CN_READER_W, CN_READER_H) != 0) {
        printf("SMOKE canary render FAIL\n");
        fail = 1;
    }
    for (g = 0; g < GUARD; g++) {
        if (base[g] != 0x5A || base[GUARD + BUF + g] != 0x5A) {
            printf("SMOKE reader OOB write FAIL\n");
            fail = 1;
            break;
        }
    }

    /* controlled re-layout via apply_config (font size 40) then back to
     * defaults; the doc stays open and current page remains valid. */
    memset(&cfg2, 0, sizeof cfg2);
    cfg2.font_path = font;
    cfg2.font_size = 40;
    if (cn_reader_apply_config(r, &cfg2) != 0 || !cn_reader_is_open(r) ||
        cn_reader_pages(r) < 1) {
        printf("SMOKE apply_config relayout FAIL\n");
        fail = 1;
    }
    if (cn_reader_pages(r) < 1) {
        printf("SMOKE relayout page count FAIL\n");
        fail = 1;
    }
    memset(&cfg2, 0, sizeof cfg2);
    cfg2.font_path = font;
    if (cn_reader_apply_config(r, &cfg2) != 0) {
        printf("SMOKE apply_config reset FAIL\n");
        fail = 1;
    }

    /* reopen after close starts at page 0 */
    cn_reader_close(r);
    if (cn_reader_is_open(r) || cn_reader_pages(r) != 0) {
        printf("SMOKE close did not clear reader FAIL\n");
        fail = 1;
    }
    if (cn_reader_open(r, valid) != 0 || cn_reader_page(r) != 0) {
        printf("SMOKE reopen does not start at page 0 FAIL\n");
        fail = 1;
    }

    free(base);
    cn_reader_free(r);
    return fail;
}

/* ---- host smoke ----------------------------------------------------- */

static int run_smoke(const char *font, const char *dir)
{
    cn_text *t = cn_text_load(font);
    cn_library *lib;
    cn_ui *ui;
    cn_canvas *c;
    cn_reader_config cfg;
    int i_valid, i_valid2, i_broken, i_book, i_txt;
    int last, pages;
    uint32_t p0_chk, p1_chk, last_chk, back_chk;
    int bad = 0, fail = 0;
    char vpath[1024], bpath[1024];

    if (!t)
        return 1;
    printf("SMOKE font=%s family=%s\n", font, cn_text_family(t));

    snprintf(vpath, sizeof vpath, "%s/valid.epub", dir);
    snprintf(bpath, sizeof bpath, "%s/broken.epub", dir);

    lib = cn_library_scan(dir);
    if (!lib) {
        fprintf(stderr, "SMOKE scan of %s failed\n", dir);
        cn_text_free(t);
        return 1;
    }
    printf("SMOKE scan count=%d\n", cn_library_count(lib));

    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    ui = cn_ui_init();
    if (!c || !ui) {
        fprintf(stderr, "SMOKE alloc failed\n");
        cn_library_free(lib);
        cn_canvas_free(c);
        cn_ui_free(ui);
        cn_text_free(t);
        return 1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.font_path = font;
    if (cn_ui_set_reader(ui, &cfg) != 0) {
        printf("SMOKE reader attach FAIL\n");
        fail = 1;
    }
    cn_ui_set_library(ui, lib);

    /* framebuffer contract stays 600x800 RGB565 / 960000 bytes. */
    if (CN_FB_W != 600 || CN_FB_H != 800 || CN_FB_W * CN_FB_H * 2 != 960000 ||
        CN_READER_W != CN_FB_W || CN_READER_H != CN_FB_H)
        bad++;
    if (cn_canvas_width(c) != 600 || cn_canvas_height(c) != 800)
        bad++;

    i_valid  = find_index(lib, "valid");
    i_valid2 = find_index(lib, "valid2");
    i_broken = find_index(lib, "broken");
    i_book   = find_index(lib, "book");
    i_txt    = find_index(lib, "notes");
    if (i_valid < 0 || i_valid2 < 0 || i_broken < 0 ||
        i_book < 0 || i_txt < 0) {
        printf("SMOKE fixture lookup FAIL\n");
        fail = 1;
    }

    /* T1. HOME -> LIBRARY via the [ Open library ] button */
    tap(ui, 200, 320);
    if (cn_ui_get_state(ui) != CN_UI_LIBRARY ||
        cn_ui_selection(ui) != 0 || cn_ui_viewport(ui) != 0)
        bad++;

    /* T2. valid EPUB activation -> UI_READER, page 0 */
    activate_index(ui, i_valid);
    if (cn_ui_get_state(ui) != CN_UI_READER)
        bad++;
    if (cn_ui_reader_pages(ui) < 2 || cn_ui_reader_page(ui) != 0)
        bad++;

    /* T3. page 0 renders non-empty */
    cn_canvas_clear(c, CN_COLOR_WHITE);
    cn_ui_render(ui, c, t);
    {
        unsigned long ink0 = ink_count(c);
        if (ink0 == 0) {
            printf("SMOKE reader page 0 blank FAIL\n");
            fail = 1;
        }
        printf("READER PAGE0 ink=%lu chk=%08x\n", ink0,
               (unsigned)checksum_fb(c));
        p0_chk = checksum_fb(c);
    }

    /* T4. NEXT changes the page (and the bitmap) */
    key(ui, CN_INPUT_PAGE_NEXT);
    if (cn_ui_reader_page(ui) != 1)
        bad++;
    cn_ui_render(ui, c, t);
    {
        unsigned long ink1 = ink_count(c);
        p1_chk = checksum_fb(c);
        if (p1_chk == p0_chk || ink1 == 0) {
            printf("SMOKE NEXT page blank/identical FAIL\n");
            fail = 1;
        }
    }

    /* T5. PREV restores page 0 bit-for-bit */
    key(ui, CN_INPUT_PAGE_PREV);
    if (cn_ui_reader_page(ui) != 0)
        bad++;
    cn_ui_render(ui, c, t);
    back_chk = checksum_fb(c);
    if (back_chk != p0_chk) {
        printf("SMOKE PREV did not restore page 0 FAIL\n");
        fail = 1;
    }

    /* T6. bounds: PREV floor on page 0; NEXT ceiling on the last page */
    key(ui, CN_INPUT_PAGE_PREV);
    if (cn_ui_reader_page(ui) != 0)
        bad++;
    pages = cn_ui_reader_pages(ui);
    while (cn_ui_reader_page(ui) < pages - 1)
        key(ui, CN_INPUT_PAGE_NEXT);
    last = cn_ui_reader_page(ui);
    if (last != pages - 1)
        bad++;
    cn_ui_render(ui, c, t);
    last_chk = checksum_fb(c);
    if (ink_count(c) == 0) {
        printf("SMOKE last page blank FAIL\n");
        fail = 1;
    }
    key(ui, CN_INPUT_PAGE_NEXT);          /* beyond last */
    if (cn_ui_reader_page(ui) != last)
        bad++;
    cn_ui_render(ui, c, t);
    if (checksum_fb(c) != last_chk) {
        printf("SMOKE NEXT past last changed page FAIL\n");
        fail = 1;
    }

    /* T7. BACK preserves library selection + viewport */
    key(ui, CN_INPUT_BACK);
    if (cn_ui_get_state(ui) != CN_UI_LIBRARY ||
        cn_ui_selection(ui) != i_valid || cn_ui_viewport(ui) != 0)
        bad++;

    /* T8. lifecycle: open A -> navigate -> close -> open B -> navigate;
     * reopening starts at page 0 */
    activate_index(ui, i_valid);
    if (cn_ui_get_state(ui) != CN_UI_READER || cn_ui_reader_page(ui) != 0)
        bad++;
    key(ui, CN_INPUT_PAGE_NEXT);
    key(ui, CN_INPUT_BACK);
    activate_index(ui, i_valid2);
    if (cn_ui_get_state(ui) != CN_UI_READER || cn_ui_reader_page(ui) != 0)
        bad++;
    key(ui, CN_INPUT_PAGE_NEXT);
    key(ui, CN_INPUT_BACK);
    if (cn_ui_get_state(ui) != CN_UI_LIBRARY ||
        cn_ui_selection(ui) != i_valid2)
        bad++;
    activate_index(ui, i_valid2);
    if (cn_ui_get_state(ui) != CN_UI_READER || cn_ui_reader_page(ui) != 0)
        bad++;

    /* T9. HOME exits the reader to HOME and closes the document */
    key(ui, CN_INPUT_HOME);
    if (cn_ui_get_state(ui) != CN_UI_HOME || cn_ui_reader_pages(ui) != 0)
        bad++;

    /* T10. invalid EPUB: deterministic SELECTED_BOOK fallback, never a
     * blank or crashed screen; BACK returns to the library */
    tap(ui, 200, 320);
    {
        activate_index(ui, i_broken);
        if (cn_ui_get_state(ui) != CN_UI_SELECTED_BOOK)
            bad++;
        if (cn_ui_selected_book(ui) == NULL ||
            cn_ui_selected_book(ui)->format != CN_BOOK_EPUB)
            bad++;
        if (cn_ui_reader_pages(ui) != 0 || cn_ui_reader_page(ui) != 0)
            bad++;
        cn_canvas_clear(c, CN_COLOR_WHITE);
        cn_ui_render(ui, c, t);
        if (ink_count(c) == 0)
            bad++;
        key(ui, CN_INPUT_BACK);
        if (cn_ui_get_state(ui) != CN_UI_LIBRARY)
            bad++;
    }

    /* T11. FB2/TXT must never enter the reader */
    activate_index(ui, i_book);
    if (cn_ui_get_state(ui) != CN_UI_SELECTED_BOOK ||
        cn_ui_selected_book(ui) == NULL ||
        cn_ui_selected_book(ui)->format != CN_BOOK_FB2)
        bad++;
    if (cn_ui_reader_pages(ui) != 0)
        bad++;
    key(ui, CN_INPUT_BACK);
    activate_index(ui, i_txt);
    if (cn_ui_get_state(ui) != CN_UI_SELECTED_BOOK ||
        cn_ui_selected_book(ui) == NULL ||
        cn_ui_selected_book(ui)->format != CN_BOOK_TXT)
        bad++;
    if (cn_ui_reader_pages(ui) != 0)
        bad++;

    /* T12. direct reader API: OOB guards, invalid open fallback, relayout,
     * reopen-at-page-0 */
    if (test_reader_direct(font, vpath, bpath) != 0)
        fail = 1;

    /* T13. long power requests a clean exit */
    key(ui, CN_INPUT_POWER_DOWN);
    if (cn_ui_exit_requested(ui))
        bad++;
    usleep(2100000);
    key(ui, CN_INPUT_POWER_UP);
    if (!cn_ui_exit_requested(ui))
        bad++;

    printf("SMOKE state=%s sel=%d top=%d bad=%d -> %s\n",
           cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_selection(ui),
           cn_ui_viewport(ui), bad,
           (bad == 0 && !fail) ? "OK" : "FAIL");
    cn_canvas_free(c);
    cn_ui_free(ui);
    cn_library_free(lib);
    cn_text_free(t);
    return (bad == 0 && !fail) ? 0 : 1;
}

/* ---- host dump modes ------------------------------------------------ */

/* Dump one frame to <outdir>/<name>.bin and append its manifest entry. */
static int dump_pixels_frame(FILE *jf, const char *outdir,
                             const char *name, const cn_canvas *c, int first)
{
    char path[1024], bin[64];
    unsigned long ink = ink_count(c);
    uint32_t chk = checksum_fb(c);

    snprintf(bin, sizeof bin, "%s.bin", name);
    snprintf(path, sizeof path, "%s/%s", outdir, bin);
    if (dump_pixels(path, c) != 0)
        return 1;
    fprintf(jf, "%s{\"frame\":\"%s\",\"file\":\"%s\","
                "\"checksum\":\"%08x\",\"ink\":%lu}",
            first ? "" : ",", name, bin, (unsigned)chk, ink);
    printf("DUMP %s chk=%08x ink=%lu -> %s\n", name, (unsigned)chk, ink, bin);
    return 0;
}

static int run_dump(const char *font, const char *dir, const char *outdir)
{
    cn_text *t = cn_text_load(font);
    cn_library *lib;
    cn_ui *ui;
    cn_canvas *c;
    cn_reader_config cfg;
    char path[1024];
    FILE *jf;
    int pages, bad = 0;
    int i_valid, i_broken, i_book;

    if (!t)
        return 1;
    lib = cn_library_scan(dir);
    if (!lib) {
        cn_text_free(t);
        return 1;
    }
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    ui = cn_ui_init();
    if (!c || !ui) {
        cn_library_free(lib);
        cn_canvas_free(c);
        cn_ui_free(ui);
        cn_text_free(t);
        return 1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.font_path = font;
    cn_ui_set_reader(ui, &cfg);
    cn_ui_set_library(ui, lib);

    i_valid  = find_index(lib, "valid");
    i_broken = find_index(lib, "broken");
    i_book   = find_index(lib, "book");
    if (i_valid < 0 || i_broken < 0 || i_book < 0)
        bad = 1;

    snprintf(path, sizeof path, "%s/dump.json", outdir);
    jf = fopen(path, "w");
    if (!jf) {
        fprintf(stderr, "dump: cannot write %s\n", path);
        cn_library_free(lib);
        cn_canvas_free(c);
        cn_ui_free(ui);
        cn_text_free(t);
        return 1;
    }
    fprintf(jf, "{\"width\":%d,\"height\":%d,\"row_size\":%d,"
                "\"frames\":[",
            CN_FB_W, CN_FB_H, CN_FB_W * 2);

    /* home */
    cn_canvas_clear(c, CN_COLOR_WHITE);
    cn_ui_render(ui, c, t);          /* state HOME */
    dump_pixels_frame(jf, outdir, "home", c, 1);

    tap(ui, 200, 320);               /* HOME -> LIBRARY */
    cn_ui_render(ui, c, t);
    dump_pixels_frame(jf, outdir, "library", c, 0);

    activate_index(ui, i_valid);     /* -> READER page 0 */
    cn_ui_render(ui, c, t);
    dump_pixels_frame(jf, outdir, "reader_p0", c, 0);
    pages = cn_ui_reader_pages(ui);

    key(ui, CN_INPUT_PAGE_NEXT);
    cn_ui_render(ui, c, t);
    dump_pixels_frame(jf, outdir, "reader_p1", c, 0);

    while (cn_ui_reader_page(ui) < pages - 1)
        key(ui, CN_INPUT_PAGE_NEXT);
    cn_ui_render(ui, c, t);
    dump_pixels_frame(jf, outdir, "reader_last", c, 0);

    key(ui, CN_INPUT_BACK);          /* -> SELECTED_BOOK via broken epub */
    activate_index(ui, i_broken);
    cn_ui_render(ui, c, t);
    dump_pixels_frame(jf, outdir, "invalid", c, 0);

    key(ui, CN_INPUT_BACK);
    activate_index(ui, i_book);      /* -> SELECTED_BOOK (FB2, no reader) */
    cn_ui_render(ui, c, t);
    dump_pixels_frame(jf, outdir, "fb2", c, 0);

    fprintf(jf, "],\"reader_pages\":%d}", pages);
    fclose(jf);

    printf("DUMP OK frames=7 reader_pages=%d\n", pages);

    cn_canvas_free(c);
    cn_ui_free(ui);
    cn_library_free(lib);
    cn_text_free(t);
    return bad;
}

/* ---- device mode ----------------------------------------------------- */

static int selected_identity(cn_ui *ui, cn_book_identity *identity)
{
    const cn_book *book = cn_ui_selected_book(ui);
    cn_book_identity_init(identity);
    if (!book || book->format != CN_BOOK_EPUB)
        return -1;
    return cn_book_identity_from_path(identity, book->path);
}

static void save_reader_progress(cn_ui *ui, cn_progress_store *store)
{
    cn_book_identity identity;
    cn_progress_record record;
    cn_progress_result result;
    const char *token;
    if (!store || cn_ui_get_state(ui) != CN_UI_READER ||
        selected_identity(ui, &identity) != 0)
        return;
    token = cn_book_identity_token(&identity);
    cn_progress_record_init(&record);
    if (cn_ui_reader_get_position(ui, &record.position) != 0) {
        fprintf(stderr, "PROGRESS save book=%s result=capture-error\n", token);
        cn_progress_record_clear(&record);
        return;
    }
    result = cn_progress_store_save(store, &identity, &record);
    fprintf(stderr, "PROGRESS save book=%s result=%s\n", token,
            cn_progress_result_name(result));
    cn_progress_record_clear(&record);
}

static void restore_reader_progress(cn_ui *ui, cn_progress_store *store)
{
    cn_book_identity identity;
    cn_progress_record record;
    cn_progress_result result;
    const char *token;
    if (!store || cn_ui_get_state(ui) != CN_UI_READER ||
        selected_identity(ui, &identity) != 0)
        return;
    token = cn_book_identity_token(&identity);
    cn_progress_record_init(&record);
    result = cn_progress_store_load(store, &identity, &record);
    fprintf(stderr, "PROGRESS load book=%s result=%s\n", token,
            cn_progress_result_name(result));
    if (result == CN_PROGRESS_OK) {
        if (cn_ui_reader_goto_position(ui, &record.position) == 0)
            fprintf(stderr, "PROGRESS restore OK\n");
        else
            fprintf(stderr, "PROGRESS restore rejected\n");
    }
    cn_progress_record_clear(&record);
}

static int run_device(const char *font_path, const char *books_dir,
                      const char *state_directory)
{
    cn_display *disp;
    cn_text *t = cn_text_load(font_path);
    cn_library *lib;
    cn_ui *ui;
    cn_canvas *c;
    cn_input *in;
    cn_reader_config cfg;
    cn_input_ev ev;
    cn_progress_store *progress = NULL;
    cn_progress_result progress_result;
    int rc;

    if (!t)
        return 1;

    lib = cn_library_scan(books_dir);
    if (!lib) {
        cn_text_free(t);
        return 1;
    }
    if (cn_library_count(lib) == 0)
        fprintf(stderr, "reader-test: %s: no books found\n", books_dir);

    progress_result = cn_progress_store_open(&progress, state_directory);
    if (progress_result != CN_PROGRESS_OK)
        fprintf(stderr, "PROGRESS open result=%s\n",
                cn_progress_result_name(progress_result));

    disp = cn_display_open();
    if (!disp) {
        cn_progress_store_close(progress);
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    ui = cn_ui_init();
    in = cn_input_open();
    if (!c || !ui || !in) {
        fprintf(stderr, "reader-test: alloc failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        cn_ui_free(ui);
        cn_progress_store_close(progress);
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    memset(&cfg, 0, sizeof cfg);
    cfg.font_path = font_path;
    if (cn_ui_set_reader(ui, &cfg) != 0) {
        fprintf(stderr, "reader-test: reader attach failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        cn_ui_free(ui);
        cn_progress_store_close(progress);
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    cn_ui_set_library(ui, lib);

    cn_ui_render(ui, c, t);
    if (cn_display_flush(disp, cn_canvas_pixels(c)) != CN_FB_BYTES) {
        fprintf(stderr, "reader-test: fb write failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        cn_ui_free(ui);
        cn_progress_store_close(progress);
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    printf("UI initial state=%s page=%d inputs=%d books=%d\n",
           cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_page(ui),
           cn_input_devices(in), cn_library_count(lib));

    for (;;) {
        cn_ui_state before;
        int redraw;
        rc = cn_input_poll(in, &ev, -1);
        if (rc < 0)
            break;
        if (rc == 0)
            continue;

        if (ev.type == CN_INPUT_TOUCH_UP)
            printf("UI touch=%d,%d\n", ev.x, ev.y);

        before = cn_ui_get_state(ui);
        if (before == CN_UI_READER &&
            (ev.type == CN_INPUT_BACK || ev.type == CN_INPUT_HOME))
            save_reader_progress(ui, progress);

        redraw = cn_ui_handle(ui, &ev);
        if (before != CN_UI_READER &&
            cn_ui_get_state(ui) == CN_UI_READER) {
            restore_reader_progress(ui, progress);
            redraw = 1;
        }

        if (redraw) {
            cn_ui_render(ui, c, t);
            if (cn_display_flush(disp, cn_canvas_pixels(c)) != CN_FB_BYTES)
                break;
            printf("UI state=%s page=%d",
                   cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_page(ui));
            if (cn_ui_get_state(ui) == CN_UI_LIBRARY ||
                cn_ui_get_state(ui) == CN_UI_SELECTED_BOOK)
                printf(" sel=%d top=%d of=%d rows=%d",
                       cn_ui_selection(ui), cn_ui_viewport(ui),
                       cn_library_count(lib), cn_ui_lib_rows());
            if (cn_ui_get_state(ui) == CN_UI_READER)
                printf(" rpages=%d rpage=%d",
                       cn_ui_reader_pages(ui), cn_ui_reader_page(ui));
            if (cn_ui_get_state(ui) == CN_UI_SELECTED_BOOK) {
                const cn_book *b = cn_ui_selected_book(ui);
                if (b)
                    printf(" title=\"%s\" format=%s", b->title,
                           cn_book_format_name(b->format));
            }
            printf("\n");
        }

        if (cn_ui_exit_requested(ui)) {
            printf("UI long-power exit\n");
            break;
        }
    }

    if (cn_ui_get_state(ui) == CN_UI_READER)
        save_reader_progress(ui, progress);
    cn_input_close(in);
    cn_canvas_free(c);
    cn_display_close(disp);
    cn_ui_free(ui);
    cn_progress_store_close(progress);
    cn_library_free(lib);
    cn_text_free(t);
    return 0;
}

/* ---- main ----------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--smoke") == 0)
        return run_smoke(argv[2], argv[3]);

    if (argc == 5 && strcmp(argv[1], "--dump") == 0)
        return run_dump(argv[2], argv[3], argv[4]);

    if (argc == 4 && argv[1][0] != '-')
        return run_device(argv[1], argv[2], argv[3]);

    fprintf(stderr,
            "usage: crossnook-reader-test <font.ttf> <books-dir> <state-dir>\n"
            "       crossnook-reader-test --smoke <font> <dir>\n"
            "       crossnook-reader-test --dump <font> <dir> <outdir>\n");
    return 2;
}
