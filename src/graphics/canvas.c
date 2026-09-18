/*
 * canvas.c — generic RGB565 drawing primitives (see canvas.h).
 *
 * Pure memory surface code: no file I/O, no hardware, no Nook specifics,
 * so the exact same module is usable for host-side tests and for the
 * framebuffer-backed UI.
 */
#include <stdlib.h>
#include <string.h>

#include "canvas.h"

cn_canvas *cn_canvas_create(int w, int h)
{
    cn_canvas *c;

    if (w <= 0 || h <= 0)
        return NULL;
    c = (cn_canvas *)malloc(sizeof *c);
    if (!c)
        return NULL;
    c->pixels = (uint16_t *)malloc((size_t)w * (size_t)h * sizeof(uint16_t));
    if (!c->pixels) {
        free(c);
        return NULL;
    }
    c->w = w;
    c->h = h;
    return c;
}

void cn_canvas_free(cn_canvas *c)
{
    if (!c)
        return;
    free(c->pixels);
    free(c);
}

int cn_canvas_width(const cn_canvas *c)
{
    return c->w;
}

int cn_canvas_height(const cn_canvas *c)
{
    return c->h;
}

uint16_t *cn_canvas_pixels(cn_canvas *c)
{
    return c->pixels;
}

void cn_canvas_clear(cn_canvas *c, uint16_t color)
{
    uint16_t *p;

    if (!c || !c->pixels)
        return;
    for (p = c->pixels; p < c->pixels + (size_t)c->w * c->h; p++)
        *p = color;
}

void cn_canvas_put_pixel(cn_canvas *c, int x, int y, uint16_t color)
{
    if (!c || x < 0 || x >= c->w || y < 0 || y >= c->h)
        return;
    c->pixels[(size_t)y * c->w + x] = color;
}

uint16_t cn_canvas_get_pixel(const cn_canvas *c, int x, int y)
{
    if (!c || x < 0 || x >= c->w || y < 0 || y >= c->h)
        return CN_COLOR_WHITE;
    return c->pixels[(size_t)y * c->w + x];
}

void cn_canvas_fill_rect(cn_canvas *c,
                         int x0, int y0, int x1, int y1,
                         uint16_t color)
{
    int x, y;

    if (!c)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    if (y1 >= c->h) y1 = c->h - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            c->pixels[(size_t)y * c->w + x] = color;
}

void cn_canvas_outline_rect(cn_canvas *c,
                            int x0, int y0, int x1, int y1,
                            uint16_t color)
{
    cn_canvas_hline(c, x0, x1, y0, color);
    cn_canvas_hline(c, x0, x1, y1, color);
    cn_canvas_vline(c, x0, y0, y1, color);
    cn_canvas_vline(c, x1, y0, y1, color);
}

void cn_canvas_hline(cn_canvas *c, int x0, int x1, int y,
                     uint16_t color)
{
    int x;

    if (!c || y < 0 || y >= c->h)
        return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    for (x = x0; x <= x1; x++)
        c->pixels[(size_t)y * c->w + x] = color;
}

void cn_canvas_vline(cn_canvas *c, int x, int y0, int y1,
                     uint16_t color)
{
    int y;

    if (!c || x < 0 || x >= c->w)
        return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 >= c->h) y1 = c->h - 1;
    for (y = y0; y <= y1; y++)
        c->pixels[(size_t)y * c->w + x] = color;
}

uint16_t cn_blend565(uint16_t back, uint16_t fore, int a)
{
    int br = (back >> 11) & 0x1F, fr = (fore >> 11) & 0x1F;
    int bg = (back >> 5) & 0x3F,  fg = (fore >> 5) & 0x3F;
    int bb = back & 0x1F,        fb = fore & 0x1F;
    int r = (br * (255 - a) + fr * a) / 255;
    int g = (bg * (255 - a) + fg * a) / 255;
    int b = (bb * (255 - a) + fb * a) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}