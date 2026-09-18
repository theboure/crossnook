/*
 * ui-library-test.c — CrossNook Library Core milestone executable.
 *
 * Composes the extracted modules + the library scanner:
 *   cn_display (sole fb0 owner), cn_canvas, cn_text, cn_input, cn_library,
 *   cn_ui (HOME / READER_TEST / LIBRARY / SELECTED_BOOK).
 *
 * Modes:
 *   crossnook-library-test <font.ttf> <books-dir>            device mode
 *   crossnook-library-test --smoke <font> <dir>              host checks
 *   crossnook-library-test --dump-library <font> <dir> <out> host library
 *   crossnook-library-test --dump-scrolled <font> <dir> <out> (scrolled)
 *   crossnook-library-test --dump-empty <font> <emptydir> <out>
 *   crossnook-library-test --dump-book <font> <dir> <out>    selected book
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
#include "ui/ui.h"

/* ---- host dump helper ------------------------------------------- */

static int dump_pixels(const char *path, const cn_canvas *c)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const unsigned char *p = (const unsigned char *)cn_canvas_pixels((cn_canvas *)c);
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

static uint32_t checksum_pixels(const cn_canvas *c)
{
    const uint16_t *px = cn_canvas_pixels((cn_canvas *)c);
    uint32_t sum = 0;
    int n = cn_canvas_width(c) * cn_canvas_height(c);
    int i;
    for (i = 0; i < n; i++)
        sum += px[i];
    return sum;
}

static void report(const char *tag, const cn_canvas *c, cn_text *t)
{
    cn_text_stats st = cn_text_get_stats(t);
    printf("%s size=%d checksum=%08x ink=%d bbox=(%d,%d)-(%d,%d)\n",
           tag, cn_canvas_width(c) * cn_canvas_height(c) * 2,
           (unsigned)checksum_pixels(c), st.ink,
           st.min_x, st.min_y, st.max_x, st.max_y);
}

static void set_touch(cn_input_ev *ev, cn_input_event type, int x, int y)
{
    memset(ev, 0, sizeof *ev);
    ev->type = type;
    ev->x = x;
    ev->y = y;
}

/* ---- smoke -------------------------------------------------------- */

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

static int run_smoke(const char *font_path, const char *dir)
{
    cn_text *t = cn_text_load(font_path);
    cn_ui *ui;
    cn_canvas *c;
    cn_input_ev ev;
    cn_library *lib;
    cn_library *empty;
    const char *probes[7];
    int probe_idx[7];
    int nprobes, i;
    int bad = 0, fail = 0;

    if (!t)
        return 1;

    printf("SMOKE font=%s family=%s\n", font_path, cn_text_family(t));

    lib = cn_library_scan(dir);
    if (!lib) {
        fprintf(stderr, "SMOKE scan of %s failed\n", dir);
        cn_text_free(t);
        return 1;
    }
    printf("SMOKE scan count=%d\n", cn_library_count(lib));

    /* 0. scanner: 41 supported fixtures ... */
    if (cn_library_count(lib) != 41) {
        printf("SMOKE scan count != 41 FAIL (got %d)\n",
               cn_library_count(lib));
        fail = 1;
    }

    /* ... deterministic sort: probes must appear in this order */
    probes[0] = "01 zone";
    probes[1] = "10 ten";
    probes[2] = "test book";
    probes[3] = "The Hobbit";
    probes[4] = "\xd0\x92\xd0\xbe\xd0\xb9\xd0\xbd\xd0\xb0 \xd0\xb8 "
                "\xd0\xbc\xd0\xb8\xd1\x80";   /* "Война и мир" */
    nprobes = 5;
    for (i = 0; i < nprobes; i++) {
        probe_idx[i] = find_index(lib, probes[i]);
        if (probe_idx[i] < 0) {
            printf("SMOKE missing probe '%s' FAIL\n", probes[i]);
            fail = 1;
        }
    }
    if (!fail) {
        for (i = 1; i < nprobes; i++) {
            if (probe_idx[i] < probe_idx[i - 1]) {
                printf("SMOKE sort order FAIL at probe %d\n", i);
                fail = 1;
            }
        }
        if (probe_idx[0] != 0) {
            printf("SMOKE first book != '01 zone' FAIL (idx %d)\n",
                   probe_idx[0]);
            fail = 1;
        }
    }
    printf("SMOKE probe order: 01zone=%d 10ten=%d testbook=%d Hobbit=%d "
           "cyr=%d\n", probe_idx[0], probe_idx[1], probe_idx[2],
           probe_idx[3], probe_idx[4]);

    /* ignored dirs / hidden / temp / unsupported tested by the exact
     * count above (the fixture directory contains those extras). */

    /* 1. empty directory scans cleanly */
    {
        char empty_path[1024];
        snprintf(empty_path, sizeof empty_path, "%s/empty", dir);
        empty = cn_library_scan(empty_path);
        if (!empty || cn_library_count(empty) != 0) {
            printf("SMOKE empty scan FAIL\n");
            fail = 1;
        }
        if (empty)
            cn_library_free(empty);
        else
            fail = 1;
    }

    ui = cn_ui_init();
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    if (!ui || !c) {
        fprintf(stderr, "SMOKE alloc failed\n");
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    cn_ui_set_library(ui, lib);

    /* 2. enter library via the HOME touch button */
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, 200, 320);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_TOUCH_UP, 200, 320);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_LIBRARY ||
        cn_ui_selection(ui) != 0 || cn_ui_viewport(ui) != 0)
        bad++;

    /* 3. PAGE_NEXT x14 -> selection crosses into the next viewport */
    for (i = 0; i < 14; i++) {
        set_touch(&ev, CN_INPUT_PAGE_NEXT, 0, 0);
        cn_ui_handle(ui, &ev);
    }
    if (cn_ui_selection(ui) != 14 || cn_ui_viewport(ui) != 14)
        bad++;

    /* 4. PAGE_PREV -> selection/scroll up */
    set_touch(&ev, CN_INPUT_PAGE_PREV, 0, 0);
    cn_ui_handle(ui, &ev);
    if (cn_ui_selection(ui) != 13 || cn_ui_viewport(ui) != 13)
        bad++;

    /* 5. touch selects row r=3 (viewport index 16), then activates */
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, 200, CN_UI_LIB_ROW_TOP + 3 * CN_UI_LIB_ROW_H + 10);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_TOUCH_UP, 200, CN_UI_LIB_ROW_TOP + 3 * CN_UI_LIB_ROW_H + 10);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_LIBRARY ||
        cn_ui_selection(ui) != 16 || cn_ui_viewport(ui) != 13)
        bad++;
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, 200, CN_UI_LIB_ROW_TOP + 3 * CN_UI_LIB_ROW_H + 10);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_TOUCH_UP, 200, CN_UI_LIB_ROW_TOP + 3 * CN_UI_LIB_ROW_H + 10);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_SELECTED_BOOK)
        bad++;
    if (cn_ui_selected_book(ui) == NULL ||
        cn_ui_selection(ui) != 16 || cn_ui_viewport(ui) != 13)
        bad++;

    /* 6. BACK keeps the same library selection/viewport */
    set_touch(&ev, CN_INPUT_BACK, 0, 0);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_LIBRARY ||
        cn_ui_selection(ui) != 16 || cn_ui_viewport(ui) != 13)
        bad++;

    /* 7. ellipsis on a very long title never draws outside the band */
    {
        cn_library *longlib = cn_library_new();
        char long_title[256];
        int k, box_ok = 1;

        memset(long_title, 'a', sizeof long_title - 1);
        long_title[sizeof long_title - 1] = '\0';
        if (cn_library_add(longlib,
                           "/tmp/books/aaaaaaaaaaaaaaaaaaaaaaa.txt",
                           "aaaaaaaaaaaaaaaaaaaaaaa.txt",
                           CN_BOOK_TXT) < 0)
            fail = 1;
        cn_ui_set_library(ui, longlib);

        /* state is still LIBRARY (set_library keeps it); render row 0,
         * whose title must be truncated with an ellipsis inside the band */
        cn_canvas_clear(c, CN_COLOR_WHITE);
        cn_ui_render(ui, c, t);
        /* first row box spans x 32..567; glyphs must not exceed the band */
        for (k = 0; k < 5; k++) {
            if (cn_canvas_get_pixel(c, CN_FB_W - 1 - k, CN_UI_LIB_ROW_TOP + 20)
                != CN_COLOR_WHITE)
                box_ok = 0;
        }
        if (!box_ok) {
            printf("SMOKE ellipsis right-edge FAIL\n");
            fail = 1;
        }
        cn_library_free(longlib);
        cn_ui_set_library(ui, lib);            /* restore main list */
    }

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

/* ---- dump modes --------------------------------------------------- */

/* Drive the UI into the requested state and dump the framebuffer. */
static int run_dump(const char *font_path, const char *dir,
                    const char *out_path, const char *mode)
{
    cn_text *t = cn_text_load(font_path);
    cn_library *lib;
    cn_ui *ui;
    cn_canvas *c;
    cn_input_ev ev;
    int i, rc = 0;

    if (!t)
        return 1;
    lib = cn_library_scan(dir);
    if (!lib) {
        cn_text_free(t);
        return 1;
    }

    ui = cn_ui_init();
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    if (!ui || !c) {
        fprintf(stderr, "dump alloc failed\n");
        cn_library_free(lib);
        cn_canvas_free(c);
        cn_ui_free(ui);
        cn_text_free(t);
        return 1;
    }
    cn_ui_set_library(ui, lib);

    /* enter library through the HOME touch button */
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, 200, 320);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_TOUCH_UP, 200, 320);
    cn_ui_handle(ui, &ev);

    if (strcmp(mode, "scrolled") == 0) {
        for (i = 0; i < 14; i++) {
            set_touch(&ev, CN_INPUT_PAGE_NEXT, 0, 0);
            cn_ui_handle(ui, &ev);
        }
    } else if (strcmp(mode, "book") == 0) {
        set_touch(&ev, CN_INPUT_TOUCH_DOWN, 200,
                  CN_UI_LIB_ROW_TOP + 10);
        cn_ui_handle(ui, &ev);
        set_touch(&ev, CN_INPUT_TOUCH_UP, 200, CN_UI_LIB_ROW_TOP + 10);
        cn_ui_handle(ui, &ev);
        set_touch(&ev, CN_INPUT_TOUCH_DOWN, 200,
                  CN_UI_LIB_ROW_TOP + 10);
        cn_ui_handle(ui, &ev);
        set_touch(&ev, CN_INPUT_TOUCH_UP, 200, CN_UI_LIB_ROW_TOP + 10);
        cn_ui_handle(ui, &ev);
    }

    cn_text_reset_stats(t);
    cn_ui_render(ui, c, t);
    if (dump_pixels(out_path, c) != 0)
        rc = 1;

    printf("DUMP mode=%s state=%s count=%d sel=%d top=%d\n",
           mode, cn_ui_state_name(cn_ui_get_state(ui)),
           cn_library_count(lib), cn_ui_selection(ui), cn_ui_viewport(ui));
    report("RENDER fb", c, t);
    fprintf(stderr, "ui-library-test: %s rendered to %s\n", mode, out_path);

    cn_canvas_free(c);
    cn_ui_free(ui);
    cn_library_free(lib);
    cn_text_free(t);
    return rc;
}

/* ---- device mode --------------------------------------------------- */

static int run_device(const char *font_path, const char *books_dir)
{
    cn_display *disp;
    cn_text *t = cn_text_load(font_path);
    cn_library *lib;
    cn_ui *ui;
    cn_canvas *c;
    cn_input *in;
    cn_input_ev ev;
    int rc;

    if (!t)
        return 1;

    lib = cn_library_scan(books_dir);
    if (!lib) {
        cn_text_free(t);
        return 1;
    }
    if (cn_library_count(lib) == 0)
        fprintf(stderr, "ui-library-test: %s: no books found\n", books_dir);

    disp = cn_display_open();
    if (!disp) {
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    ui = cn_ui_init();
    in = cn_input_open();
    if (!c || !ui || !in) {
        fprintf(stderr, "ui-library-test: alloc failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    cn_ui_set_library(ui, lib);

    cn_ui_render(ui, c, t);
    if (cn_display_flush(disp, cn_canvas_pixels(c)) != CN_FB_BYTES) {
        fprintf(stderr, "ui-library-test: fb write failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        cn_library_free(lib);
        cn_text_free(t);
        return 1;
    }
    printf("UI initial state=%s page=%d inputs=%d books=%d\n",
           cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_page(ui),
           cn_input_devices(in), cn_library_count(lib));

    for (;;) {
        rc = cn_input_poll(in, &ev, -1);
        if (rc < 0)
            break;
        if (rc == 0)
            continue;

        if (ev.type == CN_INPUT_TOUCH_UP)
            printf("UI touch=%d,%d\n", ev.x, ev.y);

        if (ev.type == CN_INPUT_MENU)
            fprintf(stderr, "ui-library-test: INPUT MENU\n");
        else if (ev.type == CN_INPUT_BACK)
            fprintf(stderr, "ui-library-test: INPUT BACK\n");
        else if (ev.type == CN_INPUT_HOME)
            fprintf(stderr, "ui-library-test: INPUT HOME\n");
        else if (ev.type == CN_INPUT_POWER_DOWN)
            fprintf(stderr, "ui-library-test: INPUT POWER_DOWN\n");
        else if (ev.type == CN_INPUT_POWER_UP)
            fprintf(stderr, "ui-library-test: INPUT POWER_UP\n");

        if (cn_ui_handle(ui, &ev)) {
            cn_ui_render(ui, c, t);
            if (cn_display_flush(disp, cn_canvas_pixels(c)) != CN_FB_BYTES)
                break;
            printf("UI state=%s page=%d\n",
                   cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_page(ui));
            if (cn_ui_get_state(ui) == CN_UI_LIBRARY ||
                cn_ui_get_state(ui) == CN_UI_SELECTED_BOOK)
                printf("UI library sel=%d top=%d of=%d rows=%d\n",
                       cn_ui_selection(ui), cn_ui_viewport(ui),
                       cn_library_count(lib), cn_ui_lib_rows());
            if (cn_ui_get_state(ui) == CN_UI_SELECTED_BOOK) {
                const cn_book *b = cn_ui_selected_book(ui);
                if (b)
                    printf("UI selected title=\"%s\" format=%s\n",
                           b->title, cn_book_format_name(b->format));
            }
        }

        if (cn_ui_exit_requested(ui)) {
            printf("UI long-power exit\n");
            break;
        }
    }

    cn_input_close(in);
    cn_canvas_free(c);
    cn_display_close(disp);
    cn_library_free(lib);
    cn_text_free(t);
    return 0;
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc == 4 && strcmp(argv[1], "--smoke") == 0)
        return run_smoke(argv[2], argv[3]);

    if (argc == 5 && strcmp(argv[1], "--dump-library") == 0)
        return run_dump(argv[2], argv[3], argv[4], "library");

    if (argc == 5 && strcmp(argv[1], "--dump-scrolled") == 0)
        return run_dump(argv[2], argv[3], argv[4], "scrolled");

    if (argc == 5 && strcmp(argv[1], "--dump-book") == 0)
        return run_dump(argv[2], argv[3], argv[4], "book");

    if (argc == 5 && strcmp(argv[1], "--dump-empty") == 0)
        return run_dump(argv[2], argv[3], argv[4], "empty");

    if (argc == 3 && argv[1][0] != '-')
        return run_device(argv[1], argv[2]);

    fprintf(stderr,
            "usage: crossnook-library-test <font.ttf> <books-dir>\n"
            "       crossnook-library-test --smoke <font> <dir>\n"
            "       crossnook-library-test --dump-library <font> <dir> <out>\n"
            "       crossnook-library-test --dump-scrolled <font> <dir> <out>\n"
            "       crossnook-library-test --dump-empty <font> <emptydir> <out>\n"
            "       crossnook-library-test --dump-book <font> <dir> <out>\n");
    return 2;
}