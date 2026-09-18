/*
 * canvas.h — generic memory-only RGB565 drawing surface. No Nook-specific
 * code here: this never touches /dev/graphics/fb0, input devices or the
 * kernel. All primitives clip to the canvas bounds.
 *
 * Pixel format: 16-bit RGB565 (high bits red, 5-6-5). A canvas is just a
 * width x height buffer of uint16_t; ownership stays with the caller via
 * create/free.
 */
#ifndef CN_CANVAS_H
#define CN_CANVAS_H

#include <stdint.h>

/* RGB565 convenience colors */
#define CN_COLOR_WHITE 0xFFFF
#define CN_COLOR_BLACK 0x0000
#define CN_COLOR_GRAY  0x7BEF

typedef struct cn_canvas {
    int       w;
    int       h;
    uint16_t *pixels;    /* w*h, row-major, row stride = w */
} cn_canvas;

/* Allocate a canvas (malloc) + optional fixed-size static variant.
 * create() returns NULL on allocation failure. */
cn_canvas *cn_canvas_create(int w, int h);
void       cn_canvas_free(cn_canvas *c);

int        cn_canvas_width(const cn_canvas *c);
int        cn_canvas_height(const cn_canvas *c);
uint16_t  *cn_canvas_pixels(cn_canvas *c);

/* Fill / sample. get_pixel() returns CN_COLOR_WHITE for out-of-bounds
 * coordinates (convenient for tests; in-bounds reads are exact). */
void       cn_canvas_clear(cn_canvas *c, uint16_t color);
void       cn_canvas_put_pixel(cn_canvas *c, int x, int y, uint16_t color);
uint16_t   cn_canvas_get_pixel(const cn_canvas *c, int x, int y);

/* Inclusive coordinates; each primitive clips silently to the canvas. */
void       cn_canvas_fill_rect(cn_canvas *c,
                               int x0, int y0, int x1, int y1,
                               uint16_t color);
void       cn_canvas_outline_rect(cn_canvas *c,
                                  int x0, int y0, int x1, int y1,
                                  uint16_t color);
void       cn_canvas_hline(cn_canvas *c, int x0, int x1, int y,
                           uint16_t color);
void       cn_canvas_vline(cn_canvas *c, int x, int y0, int y1,
                           uint16_t color);

/* Per-channel RGB565 alpha blend (validated in the text milestone):
 *   back + (fore-back) * a / 255, computed separately per 16-bit channel. */
uint16_t   cn_blend565(uint16_t back, uint16_t fore, int a);

#endif /* CN_CANVAS_H */