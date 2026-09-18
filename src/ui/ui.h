/*
 * ui.h — minimal UI state layer composing the extracted platform and
 * graphics modules. No framebuffer here: cn_ui_render draws into a
 * cn_canvas; the app decides whether/where to flush it. No FreeType
 * types, no raw input codes, no /dev paths.
 *
 * States:
 *   CN_UI_HOME        — shows CrossNook / UI Core / [ Open reader test ];
 *                       NEXT (or touch inside the button) -> READER_TEST
 *   CN_UI_READER_TEST — shows Reader test / Page N / Cyrillic sample;
 *                       NEXT/PREV change the page (floor 1),
 *                       BACK/HOME return HOME, touch draws a marker,
 *                       Power held >= 2000 ms requests exit.
 */
#ifndef CN_UI_UI_H
#define CN_UI_UI_H

#include "graphics/canvas.h"
#include "graphics/text.h"
#include "platform/nook/input.h"

#define CN_UI_POWER_LONG_MS 2000

typedef enum cn_ui_state {
    CN_UI_HOME = 0,
    CN_UI_READER_TEST
} cn_ui_state;

typedef struct cn_ui cn_ui;

cn_ui *cn_ui_init(void);
void   cn_ui_free(cn_ui *ui);

/* Feed one semantic input event. Returns non-zero when the screen
 * content changed and a redraw is warranted. */
int cn_ui_handle(cn_ui *ui, const cn_input_ev *ev);

/* Render the current state onto the canvas (text via font `t`). */
void cn_ui_render(cn_ui *ui, cn_canvas *c, cn_text *t);

/* Set true after a Power long-press completes (the app should exit). */
int cn_ui_exit_requested(const cn_ui *ui);

cn_ui_state cn_ui_get_state(const cn_ui *ui);
int         cn_ui_page(const cn_ui *ui);
const char *cn_ui_state_name(cn_ui_state s);

#endif /* CN_UI_UI_H */