/*
 * ui.h — reusable UI state layer composing the extracted platform,
 * graphics and library modules. No framebuffer here: cn_ui_render draws
 * into a cn_canvas; the app decides whether/where to flush it. No
 * FreeType types, no raw input codes, no /dev paths.
 *
 * States:
 *   CN_UI_HOME        — shows CrossNook / UI Core / [ Open reader test ]
 *                       (+ [ Open library ] when a library is attached);
 *                       NEXT (or touch on a button) opens the target
 *   CN_UI_READER_TEST — Reader test / Page N / Cyrillic sample; NEXT/PREV
 *                       change the page (floor 1), BACK/HOME return HOME,
 *                       touch draws a marker
 *   CN_UI_LIBRARY     — "CrossNook / Library" book list with highlight,
 *                       paging and touch selection/activation
 *   CN_UI_SELECTED_BOOK — diagnostic "Selected book" screen showing the
 *                       title/format/path (no parsing); reached for FB2/TXT
 *                       activation and as a deterministic fallback when an
 *                       EPUB cannot be opened; BACK returns to the same
 *                       library position/selection
 *   CN_UI_READER       — real book reader (reader/reader.h owns CREngine).
 *                       Entered by activating a selected EPUB row when a
 *                       reader is attached; the page is CREngine-rendered
 *                       full-screen (no diagnostic touch marker). NEXT/PREV
 *                       turn pages (clamped at first/last), BACK returns to
 *                       the same library position/selection, HOME returns
 *                       HOME. EPUB open failures fall back to
 *                       SELECTED_BOOK. Pages start at 0 on every open.
 *
 * Power held >= 2000 ms requests exit from any state.
 */
#ifndef CN_UI_UI_H
#define CN_UI_UI_H

#include "graphics/canvas.h"
#include "graphics/text.h"
#include "library/library.h"
#include "platform/nook/input.h"
#include "reader/reader.h"

#define CN_UI_POWER_LONG_MS 2000

/* Library list geometry (600x800 layout). Rows are a fixed 40px pitch
 * starting at y=168; the footer sits on the LIB_FOOTER_Y baseline. The
 * row count on screen derives from these three constants. */
#define CN_UI_LIB_ROW_H     40
#define CN_UI_LIB_ROW_TOP   168
#define CN_UI_LIB_FOOTER_Y  750

typedef enum cn_ui_state {
    CN_UI_HOME = 0,
    CN_UI_READER_TEST,
    CN_UI_LIBRARY,
    CN_UI_SELECTED_BOOK,
    CN_UI_READER
} cn_ui_state;

typedef struct cn_ui cn_ui;

/* Reader-only, bounded one-shot intent. UI never performs storage/network. */
typedef enum cn_ui_sync_action {
    CN_UI_SYNC_NONE = 0,
    CN_UI_SYNC_MANUAL,
    CN_UI_SYNC_USE_LOCAL,
    CN_UI_SYNC_USE_REMOTE,
    CN_UI_SYNC_CANCEL
} cn_ui_sync_action;

typedef enum cn_ui_sync_feedback {
    CN_UI_SYNC_FEEDBACK_WORKING = 0,
    CN_UI_SYNC_FEEDBACK_REMOTE_UPDATED,
    CN_UI_SYNC_FEEDBACK_REMOTE_APPLIED,
    CN_UI_SYNC_FEEDBACK_REMOTE_MISSING,
    CN_UI_SYNC_FEEDBACK_UPLOAD_POSSIBLE,
    CN_UI_SYNC_FEEDBACK_STALE,
    CN_UI_SYNC_FEEDBACK_MISMATCH,
    CN_UI_SYNC_FEEDBACK_FAILED
} cn_ui_sync_feedback;

cn_ui *cn_ui_init(void);
void   cn_ui_free(cn_ui *ui);

/* Attach a scanned library (borrowed: the UI renders from it and never
 * frees it — the caller keeps ownership). Resets selection/viewport.
 * Returns the book count. Passing NULL detaches the library. */
int cn_ui_set_library(cn_ui *ui, const cn_library *lib);

/* Attach/detach the EPUB reader (cn_ui owns it). cfg selects the
 * ReaderConfig (see reader/reader.h); pass NULL to detach. With no reader
 * attached every book activation keeps the SELECTED_BOOK diagnostic.
 * Returns 0 on success, -1 if the reader cannot be created. */
int cn_ui_set_reader(cn_ui *ui, const cn_reader_config *cfg);

/* Reader inspection (0 when no reader is attached or no document open). */
int cn_ui_reader_pages(const cn_ui *ui);
int cn_ui_reader_page(const cn_ui *ui);

/* Forward logical-position operations to the owned Reader. Persistence and
 * book identity remain application concerns; this layer never accesses the
 * filesystem. */
int cn_ui_reader_get_position(cn_ui *ui, cn_reader_position *position);
int cn_ui_reader_goto_position(cn_ui *ui,
                               const cn_reader_position *position);

/* A modal flag, not a new UI state: the real EPUB Reader stays open. The
 * caller must retain the conflict snapshot. Cancel selection is initially
 * highlighted; touch only selects, MENU confirms. Result feedback consumes
 * MENU until dismissed by BACK/HOME. */
int cn_ui_show_sync_conflict(cn_ui *ui);
int cn_ui_show_sync_feedback(cn_ui *ui, cn_ui_sync_feedback feedback);
int cn_ui_sync_modal_active(const cn_ui *ui);
int cn_ui_sync_selection(const cn_ui *ui); /* 0 local, 1 remote, 2 cancel; -1 otherwise */
cn_ui_sync_action cn_ui_take_sync_action(cn_ui *ui);

/* Number of rows that fit on the 600x800 library viewport. */
int cn_ui_lib_rows(void);

/* Feed one semantic input event. Returns non-zero when the screen
 * content changed and a redraw is warranted. */
int cn_ui_handle(cn_ui *ui, const cn_input_ev *ev);

/* Render the current state onto the canvas (text via font `t`). */
void cn_ui_render(cn_ui *ui, cn_canvas *c, cn_text *t);

/* Set true after a Power long-press completes (the app should exit). */
int cn_ui_exit_requested(const cn_ui *ui);

cn_ui_state cn_ui_get_state(const cn_ui *ui);
int         cn_ui_page(const cn_ui *ui);

/* Library state inspection (selection / viewport / book). */
const cn_library *cn_ui_library(const cn_ui *ui);
int               cn_ui_selection(const cn_ui *ui);
int               cn_ui_viewport(const cn_ui *ui);
const cn_book    *cn_ui_selected_book(const cn_ui *ui);

const char *cn_ui_state_name(cn_ui_state s);

#endif /* CN_UI_UI_H */
