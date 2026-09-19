/*
 * ui.c — reusable UI state layer (see ui.h).
 *
 * Layout conventions are the hardware-validated ones from the text
 * milestone: 32 px left/right margins, 16 px top margin, wraps after the
 * last qualifying space, line advance = 11/10 of line height. Everything
 * is drawn with canvas primitives + the text module.
 *
 * Input semantics kept from UI Core: page/menu/back/home buttons with the
 * semantic mapping, touches are committed on TOUCH_UP with the resolved
 * (last in-contact) coordinate, POWER >= 2000 ms requests exit.
 *
 * Library screen applies the following deterministic rules:
 *   - PAGE_NEXT moves the selection down one row; when the selection
 *     crosses the bottom of the viewport the viewport jumps forward so
 *     the selected row becomes its first row (clamped to the list end).
 *   - PAGE_PREV moves the selection up one row and scrolls the viewport
 *     so the selection stays visible.
 *   - Touch on a row selects it; touching the already-selected row again
 *     activates it. EPUB rows open the real reader (CN_UI_READER, page 0)
 *     when a reader is attached; a failing EPUB open and every FB2/TXT
 *     activation show the SELECTED_BOOK diagnostic screen (deterministic
 *     fallback, never a blank or crashed screen).
 *   - CN_UI_READER: NEXT/PREV turn CREngine pages (clamped at first/last),
 *     BACK returns to the library preserving selection/viewport, HOME
 *     returns HOME, POWER >= 2000 ms exits. Touches are ignored (no
 *     diagnostic marker in the real reader).
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
#define BTN_X1      340
#define BTN_R_Y0    210
#define BTN_R_Y1    252

/* "[ Open library ]" button geometry (HOME, shown when a library is set). */
#define BTN_L_Y0    300
#define BTN_L_Y1    342

#define MARK_HALF   6   /* marker box half-size */

#define MAX_LIB_CP  160 /* max codepoints considered per library row */

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

    const cn_library *lib;          /* borrowed, optional */
    int         sel;                /* selected book index */
    int         top;                /* first visible row index */

    cn_reader  *reader;             /* optional EPUB reader (owned) */

    long long   power_press_ms;     /* -1 = power not held */
    int         exit_requested;
};

/* ---- helpers ------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int btn_hit(int x, int y, int y0, int y1)
{
    return x >= BTN_X0 && x <= BTN_X1 && y >= y0 && y <= y1;
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

static void lib_down(cn_ui *ui)
{
    int rows;
    if (!ui->lib || ui->lib->count == 0)
        return;
    if (ui->sel >= ui->lib->count - 1)
        return;                             /* already at the end */
    ui->sel++;
    rows = cn_ui_lib_rows();
    if (ui->sel - ui->top >= rows) {        /* crossed viewport bottom */
        ui->top = ui->sel;
        if (ui->top > ui->lib->count - rows)
            ui->top = ui->lib->count - rows;
        if (ui->top < 0)
            ui->top = 0;
    }
}

static void lib_up(cn_ui *ui)
{
    if (!ui->lib || ui->lib->count == 0)
        return;
    if (ui->sel <= 0)
        return;
    ui->sel--;
    if (ui->sel < ui->top)
        ui->top = ui->sel;
}

/* ---- UTF-8 encode helper (deterministic ellipsis strings) --------- */

static int utf8_encode(uint32_t cp, unsigned char *out)
{
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Render text on one baseline clipped (deterministically, by width) to
 * the [left_margin, width-right_margin] band; appends "…" when it does
 * not fit. Never draws outside the band and never writes past the canvas
 * (the rasterizer also clips per glyph). */
static void render_fit(cn_text *t, cn_canvas *c, const char *text,
                       int baseline, int l, int r,
                       uint16_t fg, uint16_t bg)
{
    uint32_t cps[MAX_LIB_CP];
    unsigned char buf[MAX_LIB_CP * 4 + 4];
    int n, bad, i, k, acc, pos;
    int avail = (cn_canvas_width(c) - r) - l;
    int ell = cn_text_measure(t, 0x2026);
    uint32_t ell_cp = 0x2026;
    int total = 0;

    if (avail <= 0 || !text)
        return;
    if (ell <= 0) {                 /* font without U+2026: use "..." */
        ell = cn_text_measure(t, '.') * 3;
        ell_cp = '.';
    }
    n = cn_utf8_decode(text, cps, MAX_LIB_CP, &bad);
    if (bad > 0)
        fprintf(stderr, "ui: %d malformed byte(s) skipped\n", bad);

    for (i = 0; i < n; i++)
        total += cn_text_measure(t, cps[i]);
    if (total <= avail) {
        (void)cn_text_render(t, c, text, &baseline, l, r, fg, bg);
        return;
    }
    if (ell > avail)                /* nothing fits: draw nothing */
        return;

    k = 0;
    acc = 0;
    while (k < n) {
        int w = cn_text_measure(t, cps[k]);
        if (acc + w + ell > avail)
            break;
        acc += w;
        k++;
    }
    pos = 0;
    for (i = 0; i < k; i++) {
        int e = utf8_encode(cps[i], buf + pos);
        if (e <= 0)
            break;
        pos += e;
    }
    if (ell_cp == '.') {
        buf[pos++] = '.';
        buf[pos++] = '.';
        buf[pos++] = '.';
    } else {
        pos += utf8_encode(ell_cp, buf + pos);
    }
    buf[pos] = '\0';
    (void)cn_text_render(t, c, (const char *)buf, &baseline, l, r, fg, bg);
}

/* ---- public API --------------------------------------------------- */

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
    if (!ui)
        return;
    cn_reader_free(ui->reader);
    free(ui);
}

int cn_ui_set_library(cn_ui *ui, const cn_library *lib)
{
    if (ui) {
        ui->lib = lib;
        ui->sel = 0;
        ui->top = 0;
    }
    return (lib && ui) ? lib->count : 0;
}

int cn_ui_set_reader(cn_ui *ui, const cn_reader_config *cfg)
{
    if (!ui)
        return -1;
    if (ui->reader) {
        cn_reader_free(ui->reader);
        ui->reader = NULL;
    }
    if (cfg) {
        ui->reader = cn_reader_new(cfg);
        if (!ui->reader)
            return -1;
    }
    return 0;
}

int cn_ui_lib_rows(void)
{
    int rows = (CN_UI_LIB_FOOTER_Y - CN_UI_LIB_ROW_TOP) / CN_UI_LIB_ROW_H;
    return rows > 0 ? rows : 1;
}

int cn_ui_handle(cn_ui *ui, const cn_input_ev *ev)
{
    int redraw = 0;

    switch (ev->type) {
    case CN_INPUT_PAGE_NEXT:
        if (ui->state == CN_UI_READER) {
            if (ui->reader && cn_reader_is_open(ui->reader)) {
                int p = cn_reader_page(ui->reader);
                if (cn_reader_next(ui->reader) != p)
                    redraw = 1;
            }
        } else if (ui->state == CN_UI_LIBRARY) {
            lib_down(ui);
            redraw = 1;
        } else if (ui->state == CN_UI_HOME) {
            ui->state = CN_UI_READER_TEST;
            ui->page++;
            redraw = 1;
        } else if (ui->state == CN_UI_READER_TEST) {
            ui->page++;
            redraw = 1;
        }
        break;

    case CN_INPUT_PAGE_PREV:
        if (ui->state == CN_UI_READER) {
            if (ui->reader && cn_reader_is_open(ui->reader)) {
                int p = cn_reader_page(ui->reader);
                if (cn_reader_prev(ui->reader) != p)
                    redraw = 1;
            }
        } else if (ui->state == CN_UI_READER_TEST) {
            if (ui->page > 1)
                ui->page--;
            redraw = 1;
        } else if (ui->state == CN_UI_LIBRARY) {
            lib_up(ui);
            redraw = 1;
        }
        break;

    case CN_INPUT_MENU:
        break;                       /* reserved */

    case CN_INPUT_BACK:
        if (ui->state == CN_UI_READER) {
            if (ui->reader)
                cn_reader_close(ui->reader);
            ui->state = CN_UI_LIBRARY;
            redraw = 1;
        } else if (ui->state == CN_UI_SELECTED_BOOK) {
            ui->state = CN_UI_LIBRARY;
            redraw = 1;
        } else if (ui->state != CN_UI_HOME) {
            ui->state = CN_UI_HOME;
            redraw = 1;
        }
        break;

    case CN_INPUT_HOME:
        if (ui->state == CN_UI_READER) {
            if (ui->reader)
                cn_reader_close(ui->reader);
            ui->state = CN_UI_HOME;
            redraw = 1;
        } else if (ui->state != CN_UI_HOME) {
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
        if (ui->state != CN_UI_READER)
            mark_at(ui, ev->x, ev->y, &redraw);
        if (ui->state == CN_UI_HOME) {
            if (btn_hit(ev->x, ev->y, BTN_R_Y0, BTN_R_Y1)) {
                ui->state = CN_UI_READER_TEST;
                redraw = 1;
            } else if (ui->lib &&
                       btn_hit(ev->x, ev->y, BTN_L_Y0, BTN_L_Y1)) {
                ui->state = CN_UI_LIBRARY;
                ui->sel = 0;
                ui->top = 0;
                redraw = 1;
            }
        } else if (ui->state == CN_UI_LIBRARY) {
            int rows = cn_ui_lib_rows();
            int r = (ev->y - CN_UI_LIB_ROW_TOP) / CN_UI_LIB_ROW_H;
            if (ev->y >= CN_UI_LIB_ROW_TOP && r >= 0 && r < rows) {
                int idx = ui->top + r;
                if (ui->lib && idx < ui->lib->count) {
                    if (idx == ui->sel) {
                        const cn_book *b = &ui->lib->books[idx];
                        if (ui->reader && b->format == CN_BOOK_EPUB) {
                            if (cn_reader_open(ui->reader, b->path) == 0)
                                ui->state = CN_UI_READER;
                            else     /* deterministic fallback, never blank */
                                ui->state = CN_UI_SELECTED_BOOK;
                        } else {
                            ui->state = CN_UI_SELECTED_BOOK;
                        }
                    } else {
                        ui->sel = idx;
                    }
                    redraw = 1;
                }
            }
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
    cn_canvas_outline_rect(c, BTN_X0, BTN_R_Y0, BTN_X1, BTN_R_Y1,
                           CN_COLOR_BLACK);
    y = BTN_R_Y0 + (BTN_R_Y1 - BTN_R_Y0) / 2 + a / 2 - 6;
    (void)cn_text_render(t, c, "[ Open reader test ]", &y,
                         MARGIN_L + 16, MARGIN_R, CN_COLOR_BLACK,
                         CN_COLOR_WHITE);

    if (ui->lib) {      /* only when a library is attached */
        cn_canvas_outline_rect(c, BTN_X0, BTN_L_Y0, BTN_X1, BTN_L_Y1,
                               CN_COLOR_BLACK);
        y = BTN_L_Y0 + (BTN_L_Y1 - BTN_L_Y0) / 2 + a / 2 - 6;
        (void)cn_text_render(t, c, "[ Open library ]", &y,
                             MARGIN_L + 16, MARGIN_R, CN_COLOR_BLACK,
                             CN_COLOR_WHITE);
    }

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

static void render_library_rows(cn_ui *ui, cn_canvas *c, cn_text *t,
                                int asc)
{
    int rows = cn_ui_lib_rows();
    int r;
    int w = cn_canvas_width(c);

    for (r = 0; r < rows; r++) {
        int idx = ui->top + r;
        const cn_book *b;
        int ry0 = CN_UI_LIB_ROW_TOP + r * CN_UI_LIB_ROW_H;
        int base = ry0 + 6 + (asc > 0 ? asc : 0);
        uint16_t bg = CN_COLOR_WHITE;

        if (!ui->lib || idx >= ui->lib->count)
            break;
        b = &ui->lib->books[idx];
        if (idx == ui->sel) {
            cn_canvas_fill_rect(c, MARGIN_L, ry0,
                                w - MARGIN_R - 1, ry0 + CN_UI_LIB_ROW_H - 1,
                                CN_COLOR_GRAY);
            bg = CN_COLOR_GRAY;
        }
        render_fit(t, c, b->title, base, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, bg);
    }
}

static void render_library(cn_ui *ui, cn_canvas *c, cn_text *t)
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
    cn_text_render(t, c, "Library", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    if (!ui->lib || ui->lib->count == 0) {
        y += 18;
        a = cn_text_set_size(t, 24);
        if (a > 0)
            y += a;
        cn_text_render(t, c, "No books found.", &y, MARGIN_L, MARGIN_R,
                       CN_COLOR_BLACK, CN_COLOR_WHITE);
        return;
    }

    a = cn_text_set_size(t, 24);
    render_library_rows(ui, c, t, a);

    {
        char footer[64];
        int rows = cn_ui_lib_rows();
        int last = ui->top + rows;
        int fy;
        if (last > ui->lib->count)
            last = ui->lib->count;
        snprintf(footer, sizeof footer, "%d\xe2\x80\x93%d of %d",
                 ui->top + 1, last, ui->lib->count);
        (void)cn_text_set_size(t, 14);
        fy = CN_UI_LIB_FOOTER_Y;
        (void)cn_text_render(t, c, footer, &fy, MARGIN_L, MARGIN_R,
                             CN_COLOR_GRAY, CN_COLOR_WHITE);
    }
}

static void render_selected(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    const cn_book *b = cn_ui_selected_book(ui);
    int y = MARGIN_TOP;
    int a;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    a = cn_text_set_size(t, 36);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "Selected book", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    if (b) {
        y += 16;
        a = cn_text_set_size(t, 48);
        if (a > 0)
            y += a;
        render_fit(t, c, b->title, y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

        y += 14;
        a = cn_text_set_size(t, 24);
        if (a > 0)
            y += a;
        cn_text_render(t, c, cn_book_format_name(b->format),
                       &y, MARGIN_L, MARGIN_R,
                       CN_COLOR_BLACK, CN_COLOR_WHITE);

        y += 14;
        a = cn_text_set_size(t, 20);
        if (a > 0)
            y += a;
        render_fit(t, c, b->path, y, MARGIN_L, MARGIN_R,
                   CN_COLOR_GRAY, CN_COLOR_WHITE);
    }

    /* "[Reader not implemented]" note box spanning the content column */
    cn_canvas_outline_rect(c, BTN_X0, 440,
                           cn_canvas_width(c) - MARGIN_R - 1, 482,
                           CN_COLOR_BLACK);
    a = cn_text_set_size(t, 24);
    {
        int yy = 440 + (482 - 440) / 2 + (a > 0 ? a : 0) / 2 - 6;
        (void)cn_text_render(t, c, "[Reader not implemented]", &yy,
                             MARGIN_L + 8, MARGIN_R, CN_COLOR_GRAY,
                             CN_COLOR_WHITE);
    }
}

static void render_book(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    (void)t;   /* book pages are CREngine-rendered, not text-module drawn */
    if (ui->reader && cn_reader_is_open(ui->reader)) {
        (void)cn_reader_render(ui->reader, cn_canvas_pixels(c),
                               cn_canvas_width(c), cn_canvas_height(c));
        return;      /* full-screen page; no diagnostic touch marker */
    }
    cn_canvas_clear(c, CN_COLOR_WHITE);
}

void cn_ui_render(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    switch (ui->state) {
    case CN_UI_LIBRARY:        render_library(ui, c, t);         break;
    case CN_UI_SELECTED_BOOK:  render_selected(ui, c, t);        break;
    case CN_UI_READER:         render_book(ui, c, t);            break;
    case CN_UI_READER_TEST:    render_reader(ui, c, t);          break;
    case CN_UI_HOME:
    default:                   render_home(ui, c, t);            break;
    }
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

const cn_library *cn_ui_library(const cn_ui *ui)
{
    return ui->lib;
}

int cn_ui_selection(const cn_ui *ui)
{
    return ui->sel;
}

int cn_ui_viewport(const cn_ui *ui)
{
    return ui->top;
}

const cn_book *cn_ui_selected_book(const cn_ui *ui)
{
    if (!ui->lib || ui->sel < 0 || ui->sel >= ui->lib->count)
        return NULL;
    return &ui->lib->books[ui->sel];
}

const char *cn_ui_state_name(cn_ui_state s)
{
    switch (s) {
    case CN_UI_HOME:          return "HOME";
    case CN_UI_READER_TEST:   return "READER";
    case CN_UI_LIBRARY:       return "LIBRARY";
    case CN_UI_SELECTED_BOOK: return "SELECTED";
    case CN_UI_READER:        return "READER";
    default:                  return "?";
    }
}

int cn_ui_reader_pages(const cn_ui *ui)
{
    return (ui && ui->reader) ? cn_reader_pages(ui->reader) : 0;
}

int cn_ui_reader_page(const cn_ui *ui)
{
    return (ui && ui->reader) ? cn_reader_page(ui->reader) : 0;
}

int cn_ui_reader_get_position(cn_ui *ui, cn_reader_position *position)
{
    if (!ui || !ui->reader)
        return -1;
    return cn_reader_get_position(ui->reader, position);
}

int cn_ui_reader_goto_position(cn_ui *ui,
                               const cn_reader_position *position)
{
    if (!ui || !ui->reader)
        return -1;
    return cn_reader_goto_position(ui->reader, position);
}
