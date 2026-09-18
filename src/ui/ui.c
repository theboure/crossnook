/*
 * ui.c — minimal UI state layer (see ui.h).
 *
 * Layout conventions are the hardware-validated ones from the text
 * milestone: 32 px left/right margins, 16 px top margin, wraps after the
 * last qualifying space, line advance = 11/10 of line height. Everything
 * is drawn with canvas primitives + the text module; the reader-test page
 * counter, the touch marker and the long-press exit mirror crossnook-test.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "ui/ui.h"

#define MARGIN_L    32
#define MARGIN_R    32
#define MARGIN_TOP  16

/* "[ Open reader test ]" button geometry (HOME). */
#define BTN_X0      48
#define BTN_Y0      210
#define BTN_X1      340
#define BTN_Y1      252

#define MARK_HALF   6   /* marker box half-size */

static const char *CYR_SAMPLE =
    "\xd0\xa1\xd1\x8a\xd0\xb5\xd1\x88\xd1\x8c "
    "\xd0\xb5\xd1\x89\xd1\x91 "
    "\xd1\x8d\xd1\x82\xd0\xb8\xd1\x85 "
    "\xd0\xbc\xd1\x8f\xd0\xb3\xd0\xba\xd0\xb8\xd1\x85 "
    "\xd1\x84\xd1\x80\xd0\xb0\xd0\xbd\xd1\x86\xd1\x83\xd0\xb7"
    "\xd1\x81\xd0\xba\xd0\xb8\xd1\x85 "
    "\xd0\xb1\xd1\x83\xd0\xbb\xd0\xbe\xd0\xba, "
    "\xd0\xb4\xd0\xb0 "
    "\xd0\xb2\xd1\x8b\xd0\xbf\xd0\xb5\xd0\xb9 "
    "\xd1\x87\xd0\xb0\xd1\x8e.";

struct cn_ui {
    cn_ui_state state;
    int         page;

    int         mark_x, mark_y;     /* -1 = no marker yet */

    long long   power_press_ms;     /* -1 = power not held */
    int         exit_requested;
};

static long long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int btn_hit(int x, int y)
{
    return x >= BTN_X0 && x <= BTN_X1 && y >= BTN_Y0 && y <= BTN_Y1;
}

static void mark_at(cn_ui *ui, int x, int y, int *redraw)
{
    if (x == ui->mark_x && y == ui->mark_y)
        return;
    ui->mark_x = x;
    ui->mark_y = y;
    if (redraw)
        *redraw = 1;
}

cn_ui *cn_ui_init(void)
{
    cn_ui *ui = (cn_ui *)calloc(1, sizeof *ui);
    if (!ui)
        return NULL;
    ui->state = CN_UI_HOME;
    ui->page = 1;
    ui->mark_x = -1;
    ui->mark_y = -1;
    ui->power_press_ms = -1;
    return ui;
}

void cn_ui_free(cn_ui *ui)
{
    free(ui);
}

int cn_ui_handle(cn_ui *ui, const cn_input_ev *ev)
{
    int redraw = 0;

    switch (ev->type) {
    case CN_INPUT_PAGE_NEXT:
        if (ui->state != CN_UI_READER_TEST) {
            ui->state = CN_UI_READER_TEST;
            redraw = 1;
        }
        if (ui->state == CN_UI_READER_TEST) {
            ui->page++;
            redraw = 1;
        }
        break;

    case CN_INPUT_PAGE_PREV:
        if (ui->state == CN_UI_READER_TEST) {
            if (ui->page > 1)
                ui->page--;
            redraw = 1;
        }
        break;

    case CN_INPUT_MENU:
        break;                       /* reserved */

    case CN_INPUT_BACK:
        if (ui->state != CN_UI_HOME) {
            ui->state = CN_UI_HOME;
            redraw = 1;
        }
        break;

    case CN_INPUT_HOME:
        if (ui->state != CN_UI_HOME) {
            ui->state = CN_UI_HOME;
            redraw = 1;
        }
        break;

    case CN_INPUT_POWER_DOWN:
        ui->power_press_ms = now_ms();
        break;

    case CN_INPUT_POWER_UP:
        if (ui->power_press_ms >= 0) {
            long long dur = now_ms() - ui->power_press_ms;
            if (dur < 0)
                dur = 0;
            if (dur >= CN_UI_POWER_LONG_MS)
                ui->exit_requested = 1;
            ui->power_press_ms = -1;
        }
        break;

    case CN_INPUT_TOUCH_DOWN:
        /* provisional; finalized by TOUCH_UP */
        break;

    case CN_INPUT_TOUCH_MOVE:
        break;

    case CN_INPUT_TOUCH_UP:
        mark_at(ui, ev->x, ev->y, &redraw);
        if (ui->state == CN_UI_HOME && btn_hit(ev->x, ev->y)) {
            ui->state = CN_UI_READER_TEST;
            redraw = 1;
        }
        break;

    case CN_INPUT_NONE:
    default:
        break;
    }
    return redraw;
}

/* ---- rendering (state -> canvas) -------------------------------- */

static void draw_marker(cn_canvas *c, const cn_ui *ui)
{
    if (ui->mark_x < 0 || ui->mark_y < 0)
        return;
    cn_canvas_fill_rect(c,
                        ui->mark_x - MARK_HALF, ui->mark_y - MARK_HALF,
                        ui->mark_x + MARK_HALF, ui->mark_y + MARK_HALF,
                        CN_COLOR_BLACK);
    cn_canvas_fill_rect(c,
                        ui->mark_x - 1, ui->mark_y - 1,
                        ui->mark_x + 1, ui->mark_y + 1,
                        CN_COLOR_WHITE);
}

static void render_home(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    int y = MARGIN_TOP;
    int a;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    a = cn_text_set_size(t, 48);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "CrossNook", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    y += 14;
    a = cn_text_set_size(t, 24);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "UI Core", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    /* button */
    cn_canvas_outline_rect(c, BTN_X0, BTN_Y0, BTN_X1, BTN_Y1,
                           CN_COLOR_BLACK);
    y = BTN_Y0 + (BTN_Y1 - BTN_Y0) / 2 + a / 2 - 6;
    (void)cn_text_render(t, c, "[ Open reader test ]", &y,
                         MARGIN_L + 16, MARGIN_R, CN_COLOR_BLACK,
                         CN_COLOR_WHITE);

    draw_marker(c, ui);
}

static void render_reader(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    char page_buf[32];
    int y = MARGIN_TOP;
    int a;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    a = cn_text_set_size(t, 36);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "Reader test", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    snprintf(page_buf, sizeof page_buf, "Page %d", ui->page);
    y += 14;
    a = cn_text_set_size(t, 24);
    if (a > 0)
        y += a;
    cn_text_render(t, c, page_buf, &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    y += 14;
    a = cn_text_set_size(t, 24);
    if (a > 0)
        y += a;
    cn_text_render(t, c, CYR_SAMPLE, &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    /* footer hints */
    a = cn_text_set_size(t, 14);
    if (a > 0)
        y += a;
    {
        int fy = cn_canvas_height(c) - a - 4;
        (void)cn_text_render(t, c, "NEXT/PREV page  BACK/HOME home  PWR>2s exit",
                             &fy, MARGIN_L, MARGIN_R, CN_COLOR_GRAY,
                             CN_COLOR_WHITE);
    }

    draw_marker(c, ui);
}

void cn_ui_render(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    if (ui->state == CN_UI_HOME)
        render_home(ui, c, t);
    else
        render_reader(ui, c, t);
}

int cn_ui_exit_requested(const cn_ui *ui)
{
    return ui->exit_requested;
}

cn_ui_state cn_ui_get_state(const cn_ui *ui)
{
    return ui->state;
}

int cn_ui_page(const cn_ui *ui)
{
    return ui->page;
}

const char *cn_ui_state_name(cn_ui_state s)
{
    switch (s) {
    case CN_UI_HOME:        return "HOME";
    case CN_UI_READER_TEST: return "READER";
    default:                return "?";
    }
}