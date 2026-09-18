/*
 * text.h — reusable FreeType-based UTF-8 text renderer.
 *
 * Extracted from the hardware-validated crossnook-text diagnostic without
 * redesigning its rendering behavior. FreeType internals stay inside
 * text.c: this header exposes only the layout/render API, and the UI
 * state layer never sees FT types.
 *
 * Behavior preserved:
 *   - whole-sequence UTF-8 decoding (1..4 bytes, overlong/surrogate/
 *     >U+10FFFF/bad-continuation rejected);
 *   - grayscale (and 1-bit MONO fallback) FreeType coverage bitmap
 *     alpha-blended into the RGB565 canvas per channel;
 *   - baseline positioning from size metrics ascender;
 *   - hinted advance identical for measurement and rendering;
 *   - multiline word wrap (breaks after the last space that fits);
 *   - line spacing built into the line advance (11/10 of line height);
 *   - integer pen offsets, negative bearings, right/bottom overhang and
 *     screen clipping handled by the glyph rasterizer.
 */
#ifndef CN_GRAPHICS_TEXT_H
#define CN_GRAPHICS_TEXT_H

#include <stdint.h>

#include "canvas.h"

typedef struct cn_text cn_text;

/* Glyph-ink bookkeeping kept from the validated diagnostic (used by host
 * validation: ink pixel count + ink bbox in canvas coordinates). */
typedef struct cn_text_stats {
    int ink;
    int min_x, min_y;
    int max_x, max_y;
} cn_text_stats;

/* Load a TTF/OTF from disk (memory face, like crossnook-text). Returns
 * NULL on failure (message printed to stderr). */
cn_text *cn_text_load(const char *path);
void     cn_text_free(cn_text *t);

const char *cn_text_family(const cn_text *t);

/* Set the pixel height; returns the pixel ascent (for baselines), or -1
 * on error. */
int cn_text_set_size(cn_text *t, int px);

/* Horizontal advance (pixels) for one codepoint, matching render metrics. */
int cn_text_measure(const cn_text *t, uint32_t cp);

/* Line advance (pixels) for the current size, includes line spacing. */
int cn_text_line_advance(const cn_text *t);

/* Decode a whole NUL-terminated UTF-8 string to codepoints. Returns the
 * codepoint count (truncated at out_max); *bad receives the number of
 * malformed bytes skipped. */
int cn_utf8_decode(const char *s, uint32_t *out, int out_max, int *bad);

/* Render `utf8` as one or more word-wrapped lines onto canvas `c`,
 * starting on the baseline *baseline_y (which this call advances by the
 * line height per emitted line). left_margin / right_margin are in
 * pixels (validated layout used 32/32). Returns the number of lines
 * drawn, or -1 if text is NULL. */
int cn_text_render(cn_text *t, cn_canvas *c, const char *utf8,
                   int *baseline_y, int left_margin, int right_margin,
                   uint16_t fg, uint16_t bg);

/* Ink statistics (see cn_text_stats). Counts from the last explicit
 * reset, mirroring the diagnostic's per-run aggregation. */
void           cn_text_reset_stats(cn_text *t);
cn_text_stats  cn_text_get_stats(const cn_text *t);

#endif /* CN_GRAPHICS_TEXT_H */