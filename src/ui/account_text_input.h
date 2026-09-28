#ifndef CN_UI_ACCOUNT_TEXT_INPUT_H
#define CN_UI_ACCOUNT_TEXT_INPUT_H

#include <stddef.h>

typedef enum cn_account_text_result {
    CN_ACCOUNT_TEXT_UNCHANGED = 0,
    CN_ACCOUNT_TEXT_CHANGED,
    CN_ACCOUNT_TEXT_FULL,
    CN_ACCOUNT_TEXT_INVALID,
    CN_ACCOUNT_TEXT_DONE
} cn_account_text_result;

#define CN_ACCOUNT_TEXT_KEY_ROWS 5
#define CN_ACCOUNT_TEXT_KEY_COLUMNS 10
#define CN_ACCOUNT_TEXT_SELECTION_STRIDE 16

typedef struct cn_account_text_input {
    char *bytes;
    size_t capacity;
    size_t length;
    size_t cursor;
    int cursor_enabled;
    int symbols;
    int shifted;
    int selection;
} cn_account_text_input;

typedef enum cn_account_text_key_kind {
    CN_ACCOUNT_TEXT_KEY_INVALID = 0,
    CN_ACCOUNT_TEXT_KEY_CHARACTER,
    CN_ACCOUNT_TEXT_KEY_SHIFT,
    CN_ACCOUNT_TEXT_KEY_SYMBOLS,
    CN_ACCOUNT_TEXT_KEY_BACKSPACE,
    CN_ACCOUNT_TEXT_KEY_CURSOR_LEFT,
    CN_ACCOUNT_TEXT_KEY_CURSOR_RIGHT,
    CN_ACCOUNT_TEXT_KEY_DONE
} cn_account_text_key_kind;

typedef struct cn_account_text_key {
    cn_account_text_key_kind kind;
    unsigned char character;
    const char *label; /* non-NULL for controls and the space key */
} cn_account_text_key;

void cn_account_text_init(cn_account_text_input *input, char *bytes,
                          size_t capacity);
void cn_account_text_set_cursor_enabled(cn_account_text_input *input,
                                        int enabled);
cn_account_text_result cn_account_text_append(cn_account_text_input *input,
                                              unsigned char byte);
cn_account_text_result cn_account_text_erase(cn_account_text_input *input);
int cn_account_text_set_cursor(cn_account_text_input *input, size_t cursor);
int cn_account_text_move_cursor(cn_account_text_input *input, int step);
void cn_account_text_toggle_shift(cn_account_text_input *input);
void cn_account_text_toggle_symbols(cn_account_text_input *input);
/* Read-only canonical keyboard description shared by drawing and activation. */
int cn_account_text_key_at(const cn_account_text_input *input, int row,
                           int column, cn_account_text_key *key);
int cn_account_text_selection(const cn_account_text_input *input);
/* Select a visible key by row/column, or move selection by signed step. */
int cn_account_text_select(cn_account_text_input *input, int row, int column);
int cn_account_text_move(cn_account_text_input *input, int step);
cn_account_text_result cn_account_text_activate(cn_account_text_input *input);
/* Returns a safe byte offset at which a narrow field viewport may start. */
size_t cn_account_text_window_start(const cn_account_text_input *input,
                                    size_t visible_bytes);

#endif
