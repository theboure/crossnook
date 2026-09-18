/*
 * crossnook-text.c — FreeType TTF text renderer diagnostic (single static
 * ARM EABI binary). Renders high-quality anti-aliased UTF-8 text into the
 * proven RGB565 software framebuffer and pushes it to /dev/graphics/fb0.
 *
 * Target: Nook Simple Touch (BNRV300), kernel 2.6.29, 600x800 RGB565,
 * stride 1200. Deployment only (no rootfs / boot image changes):
 *   adb push testapp/crossnook-text /tmp/crossnook-text
 *   adb push testapp/test-font.ttf /tmp/test-font.ttf
 *   adb shell chmod 755 /tmp/crossnook-text
 *   adb shell /tmp/crossnook-text /tmp/test-font.ttf
 *
 * Host modes (run inside the pinned docker toolchain image, under qemu-arm):
 *   crossnook-text --smoke  <font.ttf>                # FT init + glyph loads
 *   crossnook-text --render <font.ttf> <out.bin>      # render, dump iframe
 *
 * Nothing here is part of the reader stack; this is a bring-up milestone for
 * FreeType text rendering only.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ft2build.h>
#include FT_FREETYPE_H

/* ---- constants ------------------------------------------------ */

#define FB_W        600
#define FB_H        800
#define FB_BYTES    (FB_W * FB_H * 2)   /* 960000 */
#define FB_PATH     "/dev/graphics/fb0"

#define COLOR_WHITE 0xFFFF
#define COLOR_BLACK 0x0000

#define MARGIN_L    32      /* left margin (px)                     */
#define MARGIN_R    32      /* right margin (px)                    */
#define MARGIN_TOP  16      /* top margin (px)                      */
#define MAX_CP      512     /* max codepoints per rendered string   */
#define LINE_NUM    11      /* internal line spacing factor:       */
#define LINE_DEN    10      /*  line advance = height * 11 / 10   */

static const char *CYR_SAMPLE =
    "\xd0\xa1\xd1\x8a\xd0\xb5\xd1\x88\xd1\x8c "                 /* Съешь */
    "\xd0\xb5\xd1\x89\xd1\x91 "                                 /* ещё */
    "\xd1\x8d\xd1\x82\xd0\xb8\xd1\x85 "                         /* этих */
    "\xd0\xbc\xd1\x8f\xd0\xb3\xd0\xba\xd0\xb8\xd1\x85 "         /* мягких */
    "\xd1\x84\xd1\x80\xd0\xb0\xd0\xbd\xd1\x86\xd1\x83\xd0\xb7"
    "\xd1\x81\xd0\xba\xd0\xb8\xd1\x85 "                         /* французских */
    "\xd0\xb1\xd1\x83\xd0\xbb\xd0\xbe\xd0\xba, "               /* булок, */
    "\xd0\xb4\xd0\xb0 "                                         /* да */
    "\xd0\xb2\xd1\x8b\xd0\xbf\xd0\xb5\xd0\xb9 "                /* выпей */
    "\xd1\x87\xd0\xb0\xd1\x8e.";                               /* чаю. */

                                        /* ---- global state ------ */

static uint16_t fb[FB_H][FB_W];   /* software RGB565 frame buffer */
static FT_Face face;              /* loaded font face */
static int  ink_px = 0;           /* pixels touched by text (for stats) */
static int  ink_min_x = FB_W, ink_min_y = FB_H;
static int  ink_max_x = -1, ink_max_y = -1;

/* ---- framebuffer helpers -------------------------------------- */

static void fb_clear(void)
{
    memset(fb, (int)0xFF, sizeof fb);
}

/* 565 channel blend: back + (fore-back) * a / 255, per 16-bit channel. */
static uint16_t blend565(uint16_t back, uint16_t fore, int a)
{
    int br = (back >> 11) & 0x1F, fr = (fore >> 11) & 0x1F;
    int bg = (back >> 5) & 0x3F,  fg = (fore >> 5) & 0x3F;
    int bb = back & 0x1F,        fb0 = fore & 0x1F;
    int r = (br * (255 - a) + fr * a) / 255;
    int g = (bg * (255 - a) + fg * a) / 255;
    int b = (bb * (255 - a) + fb0 * a) / 255;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

/* ---- UTF-8 decoding ------------------------------------------- */
/* Decodes whole sequences (1..4 bytes) to codepoints; no byte-by-byte
 * iteration at the text level. Returns bytes consumed or -1 if malformed. */

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

/* decode whole NUL-terminated string into codepoints; returns count,
 * sets *bad to the number of malformed bytes skipped. */
static int decode_utf8(const char *text, uint32_t *out, int out_max, int *bad)
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

/* ---- file + FreeType loading ---------------------------------- */

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

static int load_face_from_file(const char *path, FT_Library *lib, FT_Face *f)
{
    unsigned char *data;
    long len;
    int fd;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open font %s: %s\n", path, strerror(errno));
        return -1;
    }
    len = lseek(fd, 0, SEEK_END);
    if (len <= 0 || lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "lseek %s failed\n", path);
        close(fd);
        return -1;
    }
    data = malloc((size_t)len);
    if (!data) {
        fprintf(stderr, "malloc(%ld) failed\n", len);
        close(fd);
        return -1;
    }
    if (read_all(fd, data, (size_t)len) != 0) {
        fprintf(stderr, "read %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    if (FT_Init_FreeType(lib) != 0) {
        fprintf(stderr, "FT_Init_FreeType failed\n");
        return -1;
    }
    if (FT_New_Memory_Face(*lib, data, (FT_Long)len, 0, f) != 0) {
        fprintf(stderr, "FT_New_Memory_Face(%s) failed\n", path);
        FT_Done_FreeType(*lib);
        return -1;
    }
    if (!(*f)->family_name)
        (*f)->family_name = "?";
    return 0;
}

/* ---- glyph raster + blending ---------------------------------- */

/* Draw FreeType's coverage bitmap at the integer pen offset given by
 * dst_x/dst_y (bearing already applied). GRAY = 8-bit antialias alpha,
 * MONO = 1-bit fallback. Clipped to the screen; negative bearings and
 * right/bottom overhang are handled here. */
static void draw_glyph(const FT_Bitmap *bmp, int dst_x, int dst_y,
                       uint16_t fg, uint16_t bg)
{
    unsigned int row, col;

    for (row = 0; row < bmp->rows; row++) {
        const unsigned char *line = bmp->buffer + (size_t)row * bmp->pitch;
        int y = dst_y + row;
        int a;
        if (y < 0 || y >= FB_H)
            continue;
        for (col = 0; col < bmp->width; col++) {
            int x = dst_x + col;
            if (x < 0 || x >= FB_W)
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
            fb[y][x] = blend565(bg, fg, a);
            ink_px++;
            if (x < ink_min_x) ink_min_x = x;
            if (x > ink_max_x) ink_max_x = x;
            if (y < ink_min_y) ink_min_y = y;
            if (y > ink_max_y) ink_max_y = y;
        }
    }
}

/* Set pixel size; returns pixel ascent for baseline positioning. */
static int set_pixel_size(int px)
{
    if (FT_Set_Pixel_Sizes(face, 0, px) != 0) {
        fprintf(stderr, "FT_Set_Pixel_Sizes(%d) failed\n", px);
        exit(2);
    }
    return (face->size->metrics.ascender + 32) >> 6;
}

static int line_advance(void)
{
    int h = (face->size->metrics.height + 32) >> 6;
    return (h * LINE_NUM) / LINE_DEN;
}

/* horizontal advance (pixels) for a codepoint, matching render metrics. */
static int advance_px(uint32_t cp)
{
    FT_UInt idx = FT_Get_Char_Index(face, cp);
    if (FT_Load_Glyph(face, idx, FT_LOAD_DEFAULT | FT_LOAD_NO_BITMAP) != 0)
        return 0;
    return (int)(face->glyph->advance.x >> 6);
}

/* Render a run of codepoints on one baseline; returns final pen x. */
static int render_cps(const uint32_t *cps, int n, int baseline_y,
                      uint16_t fg, uint16_t bg)
{
    int pen_x = MARGIN_L;
    int i;

    for (i = 0; i < n; i++) {
        FT_UInt idx;
        FT_GlyphSlot slot;

        if (cps[i] == 0x20 && pen_x == MARGIN_L)
            continue;               /* drop leading spaces on a line */

        idx = FT_Get_Char_Index(face, cps[i]);
        if (FT_Load_Glyph(face, idx,
                          FT_LOAD_RENDER | FT_LOAD_DEFAULT) != 0) {
            pen_x += advance_px(cps[i]);    /* keep spacing on load error */
            continue;
        }
        slot = face->glyph;
        draw_glyph(&slot->bitmap,
                   pen_x + slot->bitmap_left,    /* negative bearings ok */
                   baseline_y - slot->bitmap_top,
                   fg, bg);
        pen_x += (int)(slot->advance.x >> 6);
    }
    return pen_x;
}

/* Render a UTF-8 string as one or more word-wrapped lines starting on the
 * current baseline *by; advances *by by the line height per emitted line.
 * Returns the number of lines drawn. */
static int render_block(const char *text, int *by, uint16_t fg, uint16_t bg)
{
    uint32_t cps[MAX_CP];
    int n, bad, lines = 0;
    int max_x = FB_W - MARGIN_R;

    n = decode_utf8(text, cps, MAX_CP, &bad);
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
                int w = advance_px(cps[i]);
                if (pen_x > 0 && pen_x + w > max_x - MARGIN_L)
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

            render_cps(&cps[p], line_end - p, *by, fg, bg);
            *by += line_advance();
            lines++;
            p = line_end;
        }
    }
    return lines;
}

/* ---- the diagnostic screen ------------------------------------- */

static void draw_screen(void)
{
    static const int sizes[] = { 18, 24, 32, 48 };
    static const char *samples[] = {
        "The quick brown fox jumps over the lazy dog.",
        "The quick brown fox jumps",
        "The quick brown fox",
        "The quick brown fox",
    };
    int y = MARGIN_TOP;
    int i;
    char label[16];

    fb_clear();

    /* title — 48 px */
    y += set_pixel_size(48);
    render_block("CrossNook", &y, COLOR_BLACK, COLOR_WHITE);

    /* font caption */
    y += set_pixel_size(14) + 4;
    render_block(face->family_name ? face->family_name : "font", &y,
                 COLOR_BLACK, COLOR_WHITE);

    /* required sample lines (Latin + Cyrillic), 24 px */
    y += set_pixel_size(24) + 4;
    render_block("The quick brown fox jumps over the lazy dog.", &y,
                 COLOR_BLACK, COLOR_WHITE);
    render_block(CYR_SAMPLE, &y, COLOR_BLACK, COLOR_WHITE);

    /* size samples at ~18/24/32/48 px */
    for (i = 0; i < 4; i++) {
        snprintf(label, sizeof label, "SIZE %d", sizes[i]);
        y += set_pixel_size(13) + 3;
        render_block(label, &y, COLOR_BLACK, COLOR_WHITE);

        y += set_pixel_size(sizes[i]) + 3;
        render_block(samples[i], &y, COLOR_BLACK, COLOR_WHITE);
    }
}

/* ---- fb write (proven path) ------------------------------------ */

static void write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "fb write: %s\n", strerror(errno));
            exit(3);
        }
        if (n == 0) {
            fprintf(stderr, "fb write: short write (0)\n");
            exit(3);
        }
        p += n;
        len -= (size_t)n;
    }
}

static void fb_refresh(int fb_fd)
{
    if (lseek(fb_fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "lseek fb: %s\n", strerror(errno));
        exit(3);
    }
    write_all(fb_fd, fb, sizeof fb);
}

/* ---- validation modes ------------------------------------------- */

/* coverage stats for one sample string */
static void coverage_report(const char *tag, const char *text)
{
    uint32_t cps[MAX_CP];
    int n, bad, i, missing = 0, total = 0;

    n = decode_utf8(text, cps, MAX_CP, &bad);
    for (i = 0; i < n; i++) {
        if (cps[i] == 0x20)
            continue;
        total++;
        if (FT_Get_Char_Index(face, cps[i]) == 0)
            missing++;
    }
    printf("%s: %d chars, %d missing, %d bad_utf8 --> %s\n",
           tag, total, missing, bad,
           (missing == 0 && bad == 0) ? "OK" : "FAIL");
}

static int run_smoke(const char *font_path)
{
    FT_Library lib = NULL;
    uint32_t cps[MAX_CP];
    int n, bad;

    if (load_face_from_file(font_path, &lib, &face) != 0)
        return 1;

    printf("SMOKE font=%s family=%s faces=%ld\n",
           font_path, face->family_name, face->num_faces);

    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        fprintf(stderr, "SMOKE no Unicode charmap\n");
        return 1;
    }
    if (set_pixel_size(24) <= 0)
        return 1;

    n = decode_utf8("The quick brown fox jumps over the lazy dog.",
                    cps, MAX_CP, &bad);
    if (n == 0 || bad != 0) {
        printf("SMOKE latin decode FAIL (n=%d bad=%d)\n", n, bad);
        return 1;
    }
    n = decode_utf8(CYR_SAMPLE, cps, MAX_CP, &bad);
    if (n == 0 || bad != 0) {
        printf("SMOKE cyrillic decode FAIL (n=%d bad=%d)\n", n, bad);
        return 1;
    }

    coverage_report("SMOKE latin", "The quick brown fox jumps over the lazy dog.");
    coverage_report("SMOKE cyrillic", CYR_SAMPLE);

    FT_Done_FreeType(lib);
    printf("SMOKE OK\n");
    return 0;
}

static int run_render(const char *font_path, const char *out_path)
{
    FT_Library lib = NULL;
    uint32_t checksum = 0;
    int fd, i, rc = 0;

    if (load_face_from_file(font_path, &lib, &face) != 0)
        return 1;
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        fprintf(stderr, "no Unicode charmap\n");
        return 1;
    }

    draw_screen();

    for (i = 0; i < FB_W * FB_H; i++)
        checksum += ((const uint16_t *)fb)[i];

    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "open %s: %s\n", out_path, strerror(errno));
        return 1;
    }
    rc = (fb_refresh(fd), 0);
    close(fd);

    printf("RENDER font=%s family=%s\n", font_path, face->family_name);
    printf("RENDER size=%d checksum=%08x ink=%d bbox=(%d,%d)-(%d,%d)\n",
           FB_BYTES, (unsigned)checksum, ink_px,
           ink_min_x, ink_min_y, ink_max_x, ink_max_y);
    return rc;
}

static int run_device(const char *font_path)
{
    FT_Library lib = NULL;
    int fb_fd;

    if (load_face_from_file(font_path, &lib, &face) != 0)
        return 1;
    if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0) {
        fprintf(stderr, "no Unicode charmap\n");
        return 1;
    }

    fb_fd = open(FB_PATH, O_WRONLY);
    if (fb_fd < 0) {
        fprintf(stderr, "open %s: %s\n", FB_PATH, strerror(errno));
        return 1;
    }

    draw_screen();
    fb_refresh(fb_fd);
    close(fb_fd);

    fprintf(stderr,
            "crossnook-text: rendered %dx%d to %s (%d bytes), sleeping\n",
            FB_W, FB_H, FB_PATH, FB_BYTES);
    for (;;)
        pause();
    return 0;   /* not reached */
}

/* ---- main ------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--smoke") == 0)
        return run_smoke(argv[2]) == 0 ? 0 : 1;

    if (argc == 4 && strcmp(argv[1], "--render") == 0)
        return run_render(argv[2], argv[3]) == 0 ? 0 : 1;

    if (argc == 2 && argv[1][0] != '-')
        return run_device(argv[1]);

    fprintf(stderr,
            "usage: crossnook-text <font.ttf>            (device mode)\n"
            "       crossnook-text --smoke <font.ttf>     (host check)\n"
            "       crossnook-text --render <font> <out>  (host render)\n");
    return 2;
}