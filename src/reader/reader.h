/*
 * reader.h — reusable EPUB reader layer (C API).
 *
 * Owns the CREngine document lifetime for the book reader. This header is
 * pure C: no CREngine or FreeType types leak out. The implementation
 * (reader.cpp) links CREngine and renders 600x800 RGB565 pages into a
 * caller-supplied buffer; CREngine never opens /dev/graphics/fb0 (only
 * src/platform/nook/display.c does, verified structurally by the build
 * scripts).
 *
 * Hard constants: the page canvas is exactly 600x800 RGB565
 * (960000 bytes, row stride 1200). A default config reproduces the
 * hardware-validated CREngine spike render for page, page by page.
 *
 * Diagnostics (deterministic, stderr):
 *   READER open path=<path>      document opened
 *   READER pages=<N>             page count after layout
 *   READER page=<N>              current 0-based page, on change only
 *   READER close                 document closed
 *   READER ERROR: ...            failures (caller decides fallback UI)
 *
 * ReaderConfig holds defaults only today (no settings UI). All-zero /
 * all-NULL leaves the proven engine defaults in place. Fields are stored
 * and re-applied through cn_reader_apply_config(), which is the ONLY way
 * future layout-affecting changes must reach CREngine (controlled
 * re-layout); UI code must never poke CREngine itself.
 *
 * Currently honored at open/apply: font_path, font_size (0 = engine
 * default 24), margin_px (0 = engine default; uniform), line_spacing_pct
 * (0 or 100 = engine default). Reserved (stored, not yet applied):
 * word_spacing_pct (the pinned CREngine has no word-spacing setter),
 * alignment, fg_color, bg_color, dark_mode, focus_reading.
 */
#ifndef CN_READER_READER_H
#define CN_READER_READER_H

#include <stdint.h>

#define CN_READER_W 600
#define CN_READER_H 800
#define CN_READER_POSITION_MAX_BYTES 65536

typedef struct cn_reader cn_reader;

/* Serializable logical document position. location is an opaque UTF-8 token:
 * callers may store/copy it, but only the reader layer may interpret it.
 * progress_10000 is secondary metadata in hundredths of one percent
 * (0..10000), or -1 when unknown; it is never used as the canonical restore
 * location.
 *
 * Initialize before first use, clear when finished. get/copy replace an
 * initialized destination and allocate its own location string. clear is
 * idempotent. A zero-initialized/empty position is safely rejected by goto. */
typedef struct cn_reader_position {
    char *location;
    int   progress_10000;
} cn_reader_position;

typedef struct cn_reader_config {
    const char *font_path;        /* TTF/OTF to register with CREngine;
                                     NULL = search the standard on-device
                                     paths (/tmp, /opt) */
    int         font_size;        /* px; 0 = engine default (24) */
    int         line_spacing_pct; /* 0 or 100 = engine default */
    int         word_spacing_pct; /* reserved (no engine setter) */
    int         margin_px;        /* uniform page margin; 0 = engine default */
    const char *alignment;        /* NULL/"" = engine default (reserved) */
    uint32_t    fg_color;         /* 0 = engine default (reserved) */
    uint32_t    bg_color;         /* 0 = engine default (reserved) */
    int         dark_mode;        /* 0 = off (reserved) */
    int         focus_reading;    /* 0 = off (reserved) */
} cn_reader_config;

/* Create a reader. cfg is copied; NULL or a zeroed struct selects the
 * validated defaults. Returns NULL on failure (e.g. font registration;
 * message on stderr). */
cn_reader *cn_reader_new(const cn_reader_config *cfg);

void cn_reader_free(cn_reader *r);

void cn_reader_position_init(cn_reader_position *position);
void cn_reader_position_clear(cn_reader_position *position);
int  cn_reader_position_copy(cn_reader_position *dst,
                             const cn_reader_position *src);

/* Replace the config. If a document is open this applies the layout
 * options and re-lays out (page count/current page refresh, current page
 * clamped). Returns 0 on success, -1 on invalid argument. */
int cn_reader_apply_config(cn_reader *r, const cn_reader_config *cfg);

/* Open an EPUB (path is used as-is). Returns 0 on success, -1 on failure;
 * on failure no document stays open. Emits READER open/pages/page. */
int  cn_reader_open(cn_reader *r, const char *path);
void cn_reader_close(cn_reader *r);
int  cn_reader_is_open(const cn_reader *r);

int cn_reader_pages(const cn_reader *r); /* 0 when closed */
int cn_reader_page(const cn_reader *r);  /* 0-based current; 0 when closed */

int cn_reader_next(cn_reader *r);        /* clamp at last page */
int cn_reader_prev(cn_reader *r);        /* clamp at page 0 */
int cn_reader_go(cn_reader *r, int page);/* clamp; returns current page */

/* Capture/restore a logical document location. The canonical location is a
 * normalized CREngine XPointer kept opaque behind this C API; page number is
 * only a derived diagnostic. goto returns -1 without moving the reader when
 * the position is empty, malformed, or cannot be resolved exactly in the
 * open document. A structurally compatible token from another document may
 * be indistinguishable; document association belongs to a future progress
 * store, not to this position value. */
int cn_reader_get_position(cn_reader *r, cn_reader_position *position);
int cn_reader_goto_position(cn_reader *r,
                            const cn_reader_position *position);

/* Render the current page into rgb565, which must point at
 * CN_READER_W*CN_READER_H*2 bytes. Clears to white first. Returns 0 on
 * success, -1 if no document is open, rgb565 is NULL, or w/h are not
 * exactly CN_READER_W/H. */
int cn_reader_render(cn_reader *r, void *rgb565, int w, int h);

#endif /* CN_READER_READER_H */
