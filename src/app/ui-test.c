/*
 * ui-test.c — CrossNook UI Core milestone executable.
 *
 * Composes the extracted modules: cn_display (only place touching
 * /dev/graphics/fb0), cn_canvas, cn_text, cn_input, cn_ui.
 *
 * Modes:
 *   crossnook-ui-test <font.ttf>                      device: fb + input + UI
 *   crossnook-ui-test --smoke <font.ttf>              host: UI logic checks
 *   crossnook-ui-test --home <font> <out.bin>         host: render HOME dump
 *   crossnook-ui-test --reader <font> <out.bin> [pg]  host: render READER dump
 *   crossnook-ui-test --primitives <out.bin>          host: canvas primitive
 *                                                     assertions + dump
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

/* ---- host modes -------------------------------------------------- */

static void set_touch(cn_input_ev *ev, cn_input_event type, int x, int y)
{
    memset(ev, 0, sizeof *ev);
    ev->type = type;
    ev->x = x;
    ev->y = y;
}

static int run_smoke(const char *font_path)
{
    cn_text *t = cn_text_load(font_path);
    cn_ui *ui;
    cn_canvas *c;
    cn_input_ev ev;
    int bad = 0;
    int fail = 0;

    if (!t)
        return 1;

    printf("SMOKE font=%s family=%s\n", font_path, cn_text_family(t));

    ui = cn_ui_init();
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    if (!ui || !c) {
        fprintf(stderr, "SMOKE alloc failed\n");
        return 1;
    }

    /* 1. HOME render has ink */
    cn_text_reset_stats(t);
    cn_ui_render(ui, c, t);
    if (cn_text_get_stats(t).ink <= 0) {
        printf("SMOKE home render ink FAIL\n");
        fail = 1;
    }
    if (cn_canvas_get_pixel(c, 10, 10) != CN_COLOR_WHITE) {
        printf("SMOKE home background FAIL\n");
        fail = 1;
    }
    if (cn_ui_get_state(ui) != CN_UI_HOME || cn_ui_page(ui) != 1)
        bad++;

    /* 2. NEXT -> READER page 2 */
    set_touch(&ev, CN_INPUT_PAGE_NEXT, 0, 0);
    if (!cn_ui_handle(ui, &ev))
        bad++;
    if (cn_ui_get_state(ui) != CN_UI_READER_TEST || cn_ui_page(ui) != 2)
        bad++;

    /* 3. NEXT -> page 3, PREV, PREV -> page 1, PREV floor stays 1 */
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_PAGE_PREV, 0, 0);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_READER_TEST || cn_ui_page(ui) != 2)
        bad++;
    cn_ui_handle(ui, &ev);
    if (cn_ui_page(ui) != 1)
        bad++;
    cn_ui_handle(ui, &ev);
    if (cn_ui_page(ui) != 1)
        bad++;

    /* 4. reader render has ink, then BACK -> HOME */
    cn_text_reset_stats(t);
    cn_canvas_clear(c, CN_COLOR_WHITE);
    cn_ui_render(ui, c, t);
    if (cn_text_get_stats(t).ink <= 0) {
        printf("SMOKE reader render ink FAIL\n");
        fail = 1;
    }
    set_touch(&ev, CN_INPUT_BACK, 0, 0);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_HOME)
        bad++;

    /* 5. HOME button hit test: tap inside -> READER, outside -> stay.
     * Taps are committed on TOUCH_UP only. */
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, 200, 230);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_HOME)
        bad++;
    set_touch(&ev, CN_INPUT_TOUCH_UP, 200, 230);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_READER_TEST)
        bad++;
    set_touch(&ev, CN_INPUT_BACK, 0, 0);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, 20, 700);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_TOUCH_UP, 20, 700);
    cn_ui_handle(ui, &ev);
    if (cn_ui_get_state(ui) != CN_UI_HOME)
        bad++;

    /* 6. touch marker commits on UP only, at (300,400) */
    set_touch(&ev, CN_INPUT_TOUCH_DOWN, 300, 400);
    cn_ui_handle(ui, &ev);
    cn_canvas_clear(c, CN_COLOR_WHITE);
    cn_ui_render(ui, c, t);
    if (cn_canvas_get_pixel(c, 294, 400) != CN_COLOR_WHITE ||
        cn_canvas_get_pixel(c, 306, 400) != CN_COLOR_WHITE) {
        printf("SMOKE marker present on DOWN FAIL\n");
        fail = 1;
    }
    set_touch(&ev, CN_INPUT_TOUCH_UP, 300, 400);
    cn_ui_handle(ui, &ev);
    cn_canvas_clear(c, CN_COLOR_WHITE);
    cn_ui_render(ui, c, t);
    if (cn_canvas_get_pixel(c, 294, 400) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 300, 400) != CN_COLOR_WHITE ||
        cn_canvas_get_pixel(c, 306, 400) != CN_COLOR_BLACK) {
        printf("SMOKE marker pixels FAIL\n");
        fail = 1;
    }

    /* 7. short power does not request exit */
    set_touch(&ev, CN_INPUT_POWER_DOWN, 0, 0);
    cn_ui_handle(ui, &ev);
    set_touch(&ev, CN_INPUT_POWER_UP, 0, 0);
    cn_ui_handle(ui, &ev);
    if (cn_ui_exit_requested(ui)) {
        printf("SMOKE short power exit FAIL\n");
        fail = 1;
    }

    printf("SMOKE state=%s page=%d bad=%d -> %s\n",
           cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_page(ui), bad,
           (bad == 0 && !fail) ? "OK" : "FAIL");
    cn_canvas_free(c);
    cn_ui_free(ui);
    cn_text_free(t);
    return (bad == 0 && !fail) ? 0 : 1;
}

static int run_render(const char *font_path, const char *out_path,
                      cn_ui_state state, int page)
{
    cn_text *t = cn_text_load(font_path);
    cn_ui *ui;
    cn_canvas *c;
    cn_input_ev ev;
    int rc = 0;

    if (!t)
        return 1;

    ui = cn_ui_init();
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    if (!ui || !c) {
        fprintf(stderr, "render alloc failed\n");
        return 1;
    }

    if (state == CN_UI_READER_TEST) {
        set_touch(&ev, CN_INPUT_PAGE_NEXT, 0, 0);
        cn_ui_handle(ui, &ev);                  /* HOME -> READER */
        while (cn_ui_page(ui) < page) {
            cn_ui_handle(ui, &ev);              /* step up to requested page */
        }
    }

    cn_text_reset_stats(t);
    cn_ui_render(ui, c, t);
    if (dump_pixels(out_path, c) != 0)
        rc = 1;

    printf("RENDER state=%s page=%d\n", cn_ui_state_name(state), page);
    report("RENDER fb", c, t);
    fprintf(stderr, "ui-test: rendered %s (%s page %d) to %s\n",
            cn_ui_state_name(state), font_path, page, out_path);

    cn_canvas_free(c);
    cn_ui_free(ui);
    cn_text_free(t);
    return rc;
}

static int run_primitives(const char *out_path)
{
    cn_canvas *c = cn_canvas_create(CN_FB_W, CN_FB_H);
    int fail = 0;

    if (!c)
        return 1;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    /* A: gray fill at top-left; nothing else may overlap this region. */
    cn_canvas_fill_rect(c, 10, 10, 20, 20, CN_COLOR_GRAY);
    if (cn_canvas_get_pixel(c, 10, 10) != CN_COLOR_GRAY ||
        cn_canvas_get_pixel(c, 20, 20) != CN_COLOR_GRAY ||
        cn_canvas_get_pixel(c, 9, 9) != CN_COLOR_WHITE ||
        cn_canvas_get_pixel(c, 21, 21) != CN_COLOR_WHITE)
        fail++;

    /* B: black rect partially off-canvas (y band 400..430 inclusive),
     * clipped on the left (starts at 0) but fully inside on the right
     * (ends at x=30). */
    cn_canvas_fill_rect(c, -10, 400, 30, 430, CN_COLOR_BLACK);
    if (cn_canvas_get_pixel(c, 0, 400) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 30, 430) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 31, 430) != CN_COLOR_WHITE)
        fail++;

    /* C: outline rect: border dark, interior clean. */
    cn_canvas_outline_rect(c, 50, 50, 80, 80, CN_COLOR_BLACK);
    if (cn_canvas_get_pixel(c, 50, 50) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 80, 80) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 65, 65) != CN_COLOR_WHITE)
        fail++;

    /* D: hline / vline. */
    cn_canvas_hline(c, 100, 200, 60, CN_COLOR_BLACK);
    cn_canvas_vline(c, 150, 90, 120, CN_COLOR_BLACK);
    if (cn_canvas_get_pixel(c, 100, 60) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 200, 60) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 201, 60) != CN_COLOR_WHITE ||
        cn_canvas_get_pixel(c, 150, 90) != CN_COLOR_BLACK ||
        cn_canvas_get_pixel(c, 150, 120) != CN_COLOR_BLACK)
        fail++;

    /* E: off-canvas hline must not draw anything. */
    cn_canvas_hline(c, -50, CN_FB_W + 50, CN_FB_H + 40, CN_COLOR_BLACK);
    if (cn_canvas_get_pixel(c, CN_FB_W - 1, 0) != CN_COLOR_WHITE)
        fail++;

    /* F: blend565: opaque, transparent, midpoint (= CN_COLOR_GRAY). */
    if (cn_blend565(CN_COLOR_WHITE, CN_COLOR_BLACK, 255) != CN_COLOR_BLACK ||
        cn_blend565(CN_COLOR_WHITE, CN_COLOR_BLACK, 0) != CN_COLOR_WHITE ||
        cn_blend565(CN_COLOR_WHITE, CN_COLOR_BLACK, 128) != CN_COLOR_GRAY)
        fail++;

    /* G: out-of-bounds get/put never touch memory. */
    cn_canvas_put_pixel(c, -5, -5, CN_COLOR_BLACK);
    cn_canvas_put_pixel(c, CN_FB_W + 5, CN_FB_H + 5, CN_COLOR_BLACK);
    if (cn_canvas_get_pixel(c, -5, -5) != CN_COLOR_WHITE ||
        cn_canvas_get_pixel(c, CN_FB_W, 0) != CN_COLOR_WHITE)
        fail++;

    printf("PRIMITIVES checksum=%08x failures=%d -> %s\n",
           (unsigned)checksum_pixels(c), fail, fail == 0 ? "OK" : "FAIL");

    if (dump_pixels(out_path, c) != 0) {
        cn_canvas_free(c);
        return 1;
    }
    cn_canvas_free(c);
    return fail == 0 ? 0 : 1;
}

/* ---- device mode -------------------------------------------------- */

static int run_device(const char *font_path)
{
    cn_display *disp;
    cn_text *t = cn_text_load(font_path);
    cn_ui *ui;
    cn_canvas *c;
    cn_input *in;
    cn_input_ev ev;
    int rc;

    if (!t)
        return 1;

    disp = cn_display_open();
    if (!disp) {
        cn_text_free(t);
        return 1;
    }
    c = cn_canvas_create(CN_FB_W, CN_FB_H);
    ui = cn_ui_init();
    in = cn_input_open();
    if (!c || !ui || !in) {
        fprintf(stderr, "ui-test: alloc failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        cn_text_free(t);
        return 1;
    }

    cn_ui_render(ui, c, t);
    if (cn_display_flush(disp, cn_canvas_pixels(c)) != CN_FB_BYTES) {
        fprintf(stderr, "ui-test: fb write failed\n");
        cn_input_close(in);
        cn_canvas_free(c);
        cn_display_close(disp);
        cn_text_free(t);
        return 1;
    }
    printf("UI initial state=%s page=%d inputs=%d\n",
           cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_page(ui),
           cn_input_devices(in));

    for (;;) {
        rc = cn_input_poll(in, &ev, -1);
        if (rc < 0)
            break;
        if (rc == 0)
            continue;

        if (ev.type == CN_INPUT_TOUCH_UP)
            printf("UI touch=%d,%d\n", ev.x, ev.y);

        if (ev.type == CN_INPUT_MENU)
            fprintf(stderr, "ui-test: INPUT MENU\n");
        else if (ev.type == CN_INPUT_BACK)
            fprintf(stderr, "ui-test: INPUT BACK\n");
        else if (ev.type == CN_INPUT_HOME)
            fprintf(stderr, "ui-test: INPUT HOME\n");
        else if (ev.type == CN_INPUT_POWER_DOWN)
            fprintf(stderr, "ui-test: INPUT POWER_DOWN\n");
        else if (ev.type == CN_INPUT_POWER_UP)
            fprintf(stderr, "ui-test: INPUT POWER_UP\n");

        if (cn_ui_handle(ui, &ev)) {
            cn_ui_render(ui, c, t);
            if (cn_display_flush(disp, cn_canvas_pixels(c)) != CN_FB_BYTES)
                break;
            printf("UI state=%s page=%d\n",
                   cn_ui_state_name(cn_ui_get_state(ui)), cn_ui_page(ui));
        }

        if (cn_ui_exit_requested(ui)) {
            printf("UI long-power exit\n");
            break;
        }
    }

    cn_input_close(in);
    cn_canvas_free(c);
    cn_display_close(disp);
    cn_text_free(t);
    return 0;
}

/* ---- main --------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--smoke") == 0)
        return run_smoke(argv[2]);

    if (argc == 4 && strcmp(argv[1], "--home") == 0)
        return run_render(argv[2], argv[3], CN_UI_HOME, 1);

    if (argc >= 4 && argv[1] != NULL && strcmp(argv[1], "--reader") == 0) {
        int page = (argc >= 5) ? atoi(argv[4]) : 1;
        if (page < 1)
            page = 1;
        return run_render(argv[2], argv[3], CN_UI_READER_TEST, page);
    }

    if (argc == 3 && strcmp(argv[1], "--primitives") == 0)
        return run_primitives(argv[2]);

    if (argc == 2 && argv[1][0] != '-')
        return run_device(argv[1]);

    fprintf(stderr,
            "usage: crossnook-ui-test <font.ttf>            (device mode)\n"
            "       crossnook-ui-test --smoke <font.ttf>    (host check)\n"
            "       crossnook-ui-test --home <font> <out>   (host render)\n"
            "       crossnook-ui-test --reader <font> <out> [page]\n"
            "       crossnook-ui-test --primitives <out>    (host check)\n");
    return 2;
}