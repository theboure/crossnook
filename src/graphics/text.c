/*
 * text.c — reusable FreeType-based UTF-8 text renderer.
 *
 * Faithful extraction of the hardware-validated crossnook-text diagnostic
 * (tag milestone/freetype-text-rendering); rendering behavior is unchanged:
 *
 *   - whole-sequence UTF-8 decoding (overlong / surrogate / >U+10FFFF /
 *     bad-continuation rejected);
 *   - coverage bitmaps alpha-blended per RGB565 channel (cn_blend565);
 *   - baseline = ascender from size metrics;
 *   - hinted advance for both measurement and rendering;
 *   - word wrap breaks after the last space that fits (wrap only on real
 *     width overflow), leading spaces dropped, progress guaranteed;
 *   - integer pen offsets; negative bearings and right/bottom overhang
 *     clipped in the rasterizer;
 *   - line advance = round(height) * 11 / 10 (LINE_NUM/LINE_DEN).
 *
 * FreeType internals are confined to this file.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "text.h"

#define MAX_CP      512     /* max codepoints per rendered string */

#define LINE_NUM    11      /* line spacing factor:          */
#define LINE_DEN    10      /*  advance = round(height) * LINE_NUM / LINE_DEN */

struct cn_text {
    FT_Library  lib;
    FT_Face     face;
    unsigned char *data;    /* memory face payload (owned) */
    long        data_len;
    cn_text_stats stats;
};

/* ---- UTF-8 decoding (from crossnook-text.c, unchanged) ---------- */

static int utf8_next(const unsigned char *s, uint32_t *cp)
{
    unsigned char c = s[0];
    uint32_t v;
    int n, i;

    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if ((c & 0xE0) == 0xC0) {
        v = c & 0x1F; n = 2;
    } else if ((c & 0xF0) == 0xE0) {
        v = c & 0x0F; n = 3;
    } else if ((c & 0xF8) == 0xF0) {
        v = c & 0x07; n = 4;
    } else {
        return -1;                  /* stray continuation / out of range */
    }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80)
            return -1;              /* bad continuation byte */
        v = (v << 6) | (s[i] & 0x3F);
    }
    if (v < (n == 2 ? 0x80U : n == 3 ? 0x800U : 0x10000U))
        return -1;                  /* overlong encoding */
    if (v > 0x10FFFF)
        return -1;
    if (v >= 0xD800 && v <= 0xDFFF)
        return -1;                  /* UTF-16 surrogate */
    *cp = v;
    return n;
}

int cn_utf8_decode(const char *text, uint32_t *out, int out_max, int *bad)
{
    const unsigned char *p = (const unsigned char *)text;
    int n = 0;
    int inv = 0;

    *bad = 0;
    while (*p) {
        uint32_t cp;
        int r = utf8_next(p, &cp);
        if (r < 0) {
            inv++;
            p++;                    /* skip the offending byte */
            continue;
        }
        if (n < out_max)
            out[n] = cp;
        n++;
        p += r;
    }
    *bad = inv;
    return n;
}

/* ---- file + FreeType loading ----------------------------------- */

static int read_all(int fd, void *buf, size_t len)
{
    unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int load_data_from_file(const char *path, unsigned char **data,
                               long *len)
{
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open font %s: %s\n", path, strerror(errno));
        return -1;
    }
    *len = lseek(fd, 0, SEEK_END);
    if (*len <= 0 || lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "lseek %s failed\n", path);
        close(fd);
        return -1;
    }
    *data = malloc((size_t)*len);
    if (!*data) {
        fprintf(stderr, "malloc(%ld) failed\n", *len);
        close(fd);
        return -1;
    }
    if (read_all(fd, *data, (size_t)*len) != 0) {
        fprintf(stderr, "read %s: %s\n", path, strerror(errno));
        free(*data);
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

cn_text *cn_text_load(const char *path)
{
    cn_text *t;
    unsigned char *data;
    long len;

    if (load_data_from_file(path, &data, &len) != 0)
        return NULL;

    t = (cn_text *)calloc(1, sizeof *t);
    if (!t) {
        free(data);
        return NULL;
    }
    t->data = data;
    t->data_len = len;

    if (FT_Init_FreeType(&t->lib) != 0) {
        fprintf(stderr, "FT_Init_FreeType failed\n");
        goto fail;
    }
    if (FT_New_Memory_Face(t->lib, data, (FT_Long)len, 0, &t->face) != 0) {
        fprintf(stderr, "FT_New_Memory_Face(%s) failed\n", path);
        FT_Done_FreeType(t->lib);
        goto fail;
    }
    if (!t->face->family_name)
        t->face->family_name = "?";
    if (FT_Select_Charmap(t->face, FT_ENCODING_UNICODE) != 0) {
        fprintf(stderr, "no Unicode charmap\n");
        FT_Done_Face(t->face);
        FT_Done_FreeType(t->lib);
        goto fail;
    }
    return t;

fail:
    free(t);
    return NULL;
}

void cn_text_free(cn_text *t)
{
    if (!t)
        return;
    FT_Done_Face(t->face);
    FT_Done_FreeType(t->lib);
    free(t->data);
    free(t);
}

const char *cn_text_family(const cn_text *t)
{
    return t->face->family_name ? t->face->family_name : "?";
}

/* ---- glyph raster + blending ----------------------------------- */

int cn_text_set_size(cn_text *t, int px)
{
    if (FT_Set_Pixel_Sizes(t->face, 0, px) != 0) {
        fprintf(stderr, "FT_Set_Pixel_Sizes(%d) failed\n", px);
        return -1;
    }
    return (t->face->size->metrics.ascender + 32) >> 6;
}

int cn_text_line_advance(const cn_text *t)
{
    int h = (t->face->size->metrics.height + 32) >> 6;
    return (h * LINE_NUM) / LINE_DEN;
}

/* horizontal advance (pixels) for a codepoint, matching render metrics. */
int cn_text_measure(const cn_text *t, uint32_t cp)
{
    FT_UInt idx = FT_Get_Char_Index(t->face, cp);
    if (FT_Load_Glyph(t->face, idx, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) != 0)
        return 0;
    return (int)(t->face->glyph->advance.x >> 6);
}

/* Draw FreeType's coverage bitmap at the integer pen offset given by
 * dst_x/dst_y (bearing already applied). GRAY = 8-bit antialias alpha,
 * MONO = 1-bit fallback. Clipped to the canvas; negative bearings and
 * right/bottom overhang handled here (from crossnook-text.c). */
static void draw_glyph(cn_text *t, cn_canvas *c,
                       const FT_Bitmap *bmp, int dst_x, int dst_y,
                       uint16_t fg, uint16_t bg)
{
    unsigned int row, col;

    for (row = 0; row < bmp->rows; row++) {
        const unsigned char *line = bmp->buffer + (size_t)row * bmp->pitch;
        int y = dst_y + row;
        int a;
        if (y < 0 || y >= cn_canvas_height(c))
            continue;
        for (col = 0; col < bmp->width; col++) {
            int x = dst_x + col;
            if (x < 0 || x >= cn_canvas_width(c))
                continue;
            if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY) {
                a = line[col];
            } else if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
                a = (line[col >> 3] >> (7 - (col & 7))) & 1 ? 255 : 0;
            } else {
                continue;           /* LCD / other: not expected */
            }
            if (a == 0)
                continue;
            cn_canvas_put_pixel(c, x, y, cn_blend565(bg, fg, a));
            if (t->stats.ink == 0) {
                t->stats.min_x = x;
                t->stats.min_y = y;
                t->stats.max_x = x;
                t->stats.max_y = y;
            } else {
                if (x < t->stats.min_x) t->stats.min_x = x;
                if (x > t->stats.max_x) t->stats.max_x = x;
                if (y < t->stats.min_y) t->stats.min_y = y;
                if (y > t->stats.max_y) t->stats.max_y = y;
            }
            t->stats.ink++;
        }
    }
}

void cn_text_reset_stats(cn_text *t)
{
    t->stats.ink = 0;
    t->stats.min_x = INT_MAX;
    t->stats.min_y = INT_MAX;
    t->stats.max_x = -1;
    t->stats.max_y = -1;
}

cn_text_stats cn_text_get_stats(const cn_text *t)
{
    return t->stats;
}

/* Render one run of codepoints on a baseline; returns final pen x. */
static int render_cps(cn_text *t, cn_canvas *c,
                      const uint32_t *cps, int n, int baseline_y,
                      int left_margin, uint16_t fg, uint16_t bg)
{
    int pen_x = left_margin;
    int i;

    for (i = 0; i < n; i++) {
        FT_UInt idx;
        FT_GlyphSlot slot;

        if (cps[i] == 0x20 && pen_x == left_margin)
            continue;               /* drop leading spaces on a line */

        idx = FT_Get_Char_Index(t->face, cps[i]);
        if (FT_Load_Glyph(t->face, idx,
                          FT_LOAD_RENDER | FT_LOAD_DEFAULT) != 0) {
            pen_x += cn_text_measure(t, cps[i]);   /* keep spacing on error */
            continue;
        }
        slot = t->face->glyph;
        draw_glyph(t, c,
                   &slot->bitmap,
                   pen_x + slot->bitmap_left,      /* negative bearings ok */
                   baseline_y - slot->bitmap_top,
                   fg, bg);
        pen_x += (int)(slot->advance.x >> 6);
    }
    return pen_x;
}

int cn_text_render(cn_text *t, cn_canvas *c, const char *utf8,
                   int *baseline_y, int left_margin, int right_margin,
                   uint16_t fg, uint16_t bg)
{
    uint32_t cps[MAX_CP];
    int n, bad, lines = 0;
    int max_x = cn_canvas_width(c) - right_margin;

    if (!utf8)
        return -1;

    n = cn_utf8_decode(utf8, cps, MAX_CP, &bad);
    if (bad > 0)
        fprintf(stderr, "utf8: %d malformed byte(s) skipped\n", bad);

    {
        int p = 0;
        while (p < n) {
            int i, pen_x, line_end;
            int last_space = -1;

            pen_x = 0;
            i = p;
            while (i < n) {
                int w = cn_text_measure(t, cps[i]);
                if (pen_x > 0 && pen_x + w > max_x - left_margin)
                    break;              /* would overflow the line */
                pen_x += w;
                if (cps[i] == 0x20)
                    last_space = i;     /* remember a valid break point */
                i++;
            }
            line_end = i;
            if (i < n && last_space > p && last_space + 1 < i)
                line_end = last_space + 1;   /* wrap after the last space */
            if (line_end == p)
                line_end = p + 1;            /* safety: ensure progress */

            render_cps(t, c, &cps[p], line_end - p, *baseline_y,
                       left_margin, fg, bg);
            *baseline_y += cn_text_line_advance(t);
            lines++;
            p = line_end;
        }
    }
    return lines;
}