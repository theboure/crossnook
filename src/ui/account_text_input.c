#include <string.h>
#include "ui/account_text_input.h"

static const char *const lower_rows[3] = {
    "1234567890", "qwertyuiop", "asdfghjkl"
};
static const char *const upper_rows[3] = {
    "1234567890", "QWERTYUIOP", "ASDFGHJKL"
};
static const char *const symbol_rows[3] = {
    "1234567890", "+-=^_@#$%&", "!?:;'\"()*`"
};

static void character_key(cn_account_text_key *key, unsigned char character)
{
    key->kind = CN_ACCOUNT_TEXT_KEY_CHARACTER;
    key->character = character;
    key->label = character == ' ' ? "SPACE" : NULL;
}

void cn_account_text_init(cn_account_text_input *in, char *bytes,
                          size_t capacity)
{
    if (!in) return;
    memset(in, 0, sizeof *in);
    in->bytes = bytes;
    in->capacity = capacity;
    in->cursor_enabled = 1;
    if (bytes && capacity) {
        bytes[0] = 0;
        in->selection = 0;
    }
}

void cn_account_text_set_cursor_enabled(cn_account_text_input *in, int enabled)
{
    if (!in) return;
    in->cursor_enabled = enabled != 0;
    if (!in->cursor_enabled) in->cursor = in->length;
}

cn_account_text_result cn_account_text_append(cn_account_text_input *in,
                                             unsigned char byte)
{
    if (!in || !in->bytes || !in->capacity || byte < 0x20 || byte > 0x7e)
        return CN_ACCOUNT_TEXT_INVALID;
    if (!in->cursor_enabled) in->cursor = in->length;
    if (in->cursor > in->length) return CN_ACCOUNT_TEXT_INVALID;
    if (in->length + 1 >= in->capacity) return CN_ACCOUNT_TEXT_FULL;
    memmove(in->bytes + in->cursor + 1, in->bytes + in->cursor,
            in->length - in->cursor + 1);
    in->bytes[in->cursor++] = (char)byte;
    ++in->length;
    return CN_ACCOUNT_TEXT_CHANGED;
}

cn_account_text_result cn_account_text_erase(cn_account_text_input *in)
{
    if (!in || !in->bytes || in->cursor > in->length)
        return CN_ACCOUNT_TEXT_INVALID;
    if (!in->cursor_enabled) in->cursor = in->length;
    if (!in->cursor) return CN_ACCOUNT_TEXT_UNCHANGED;
    memmove(in->bytes + in->cursor - 1, in->bytes + in->cursor,
            in->length - in->cursor + 1);
    --in->cursor;
    --in->length;
    return CN_ACCOUNT_TEXT_CHANGED;
}

int cn_account_text_set_cursor(cn_account_text_input *in, size_t cursor)
{
    if (!in || !in->cursor_enabled || cursor > in->length) return 0;
    if (in->cursor == cursor) return 0;
    in->cursor = cursor;
    return 1;
}

int cn_account_text_move_cursor(cn_account_text_input *in, int step)
{
    if (!in || !in->cursor_enabled || !step) return 0;
    if (step < 0) {
        if (!in->cursor) return 0;
        --in->cursor;
    } else {
        if (in->cursor >= in->length) return 0;
        ++in->cursor;
    }
    return 1;
}

void cn_account_text_toggle_shift(cn_account_text_input *in)
{ if (in) in->shifted = !in->shifted; }
void cn_account_text_toggle_symbols(cn_account_text_input *in)
{
    if (!in) return;
    in->symbols = !in->symbols;
    in->shifted = 0;
}

int cn_account_text_key_at(const cn_account_text_input *in, int row, int column,
                           cn_account_text_key *key)
{
    const char *row_chars;
    size_t length;
    if (!in || !key || row < 0 || row >= CN_ACCOUNT_TEXT_KEY_ROWS ||
        column < 0 || column >= CN_ACCOUNT_TEXT_KEY_COLUMNS)
        return 0;
    memset(key, 0, sizeof *key);

    if (row < 3) {
        row_chars = in->symbols ? symbol_rows[row] :
                    (in->shifted ? upper_rows[row] : lower_rows[row]);
        length = strlen(row_chars);
        if ((size_t)column >= length) return 0;
        character_key(key, (unsigned char)row_chars[column]);
        return 1;
    }

    if (!in->symbols && row == 3) {
        static const char letters[] = "zxcvbnm";
        if (column == 0) {
            key->kind = CN_ACCOUNT_TEXT_KEY_SHIFT;
            key->label = "SHIFT";
            return 1;
        }
        if (column >= 1 && column <= 7) {
            unsigned char c = (unsigned char)letters[column - 1];
            character_key(key, in->shifted ? (unsigned char)(c - 'a' + 'A') : c);
            return 1;
        }
        if (column == 8) {
            key->kind = CN_ACCOUNT_TEXT_KEY_BACKSPACE;
            key->label = "DEL";
            return 1;
        }
        return 0;
    }

    if (in->symbols && row == 3) {
        static const char punctuation[] = "[]{}<>\\|~";
        if (column >= 0 && column <= 8) {
            character_key(key, (unsigned char)punctuation[column]);
            return 1;
        }
        if (column == 9) {
            key->kind = CN_ACCOUNT_TEXT_KEY_BACKSPACE;
            key->label = "DEL";
            return 1;
        }
        return 0;
    }

    if (row == 4) {
        if (column == 0) {
            key->kind = CN_ACCOUNT_TEXT_KEY_SYMBOLS;
            key->label = in->symbols ? "ABC" : "#+=";
        } else if (column == 1) {
            character_key(key, '/');
        } else if (column == 2 && in->cursor_enabled) {
            key->kind = CN_ACCOUNT_TEXT_KEY_CURSOR_LEFT;
            key->label = "\xe2\x86\x90";
        } else if (column == 3) {
            character_key(key, ' ');
        } else if (column == 4 && in->cursor_enabled) {
            key->kind = CN_ACCOUNT_TEXT_KEY_CURSOR_RIGHT;
            key->label = "\xe2\x86\x92";
        } else if (column == 5) {
            character_key(key, in->symbols ? ',' : '.');
        }
        else if (column == 6) {
            key->kind = CN_ACCOUNT_TEXT_KEY_DONE;
            key->label = "DONE";
        } else return 0;
        return 1;
    }
    return 0;
}

int cn_account_text_selection(const cn_account_text_input *in)
{ return in ? in->selection : -1; }

int cn_account_text_select(cn_account_text_input *in, int row, int column)
{
    cn_account_text_key key;
    if (!in || !cn_account_text_key_at(in, row, column, &key)) return -1;
    in->selection = row * CN_ACCOUNT_TEXT_SELECTION_STRIDE + column;
    return in->selection;
}

int cn_account_text_move(cn_account_text_input *in, int step)
{
    int tries;
    if (!in || !step) return in ? in->selection : -1;
    for (tries = 0; tries < CN_ACCOUNT_TEXT_KEY_ROWS *
                              CN_ACCOUNT_TEXT_SELECTION_STRIDE; ++tries) {
        int row, col;
        cn_account_text_key key;
        int total = CN_ACCOUNT_TEXT_KEY_ROWS * CN_ACCOUNT_TEXT_SELECTION_STRIDE;
        in->selection = (in->selection + (step > 0 ? 1 : total - 1)) % total;
        row = in->selection / CN_ACCOUNT_TEXT_SELECTION_STRIDE;
        col = in->selection % CN_ACCOUNT_TEXT_SELECTION_STRIDE;
        if (cn_account_text_key_at(in, row, col, &key))
            return in->selection;
    }
    return in->selection;
}

cn_account_text_result cn_account_text_activate(cn_account_text_input *in)
{
    int row, col;
    cn_account_text_key key;
    if (!in || in->selection < 0 ||
        in->selection >= CN_ACCOUNT_TEXT_KEY_ROWS *
                         CN_ACCOUNT_TEXT_SELECTION_STRIDE)
        return CN_ACCOUNT_TEXT_INVALID;
    row = in->selection / CN_ACCOUNT_TEXT_SELECTION_STRIDE;
    col = in->selection % CN_ACCOUNT_TEXT_SELECTION_STRIDE;
    if (!cn_account_text_key_at(in, row, col, &key))
        return CN_ACCOUNT_TEXT_UNCHANGED;
    switch (key.kind) {
    case CN_ACCOUNT_TEXT_KEY_CHARACTER: {
        cn_account_text_result result = cn_account_text_append(in, key.character);
        if (result == CN_ACCOUNT_TEXT_CHANGED && in->shifted &&
            ((key.character >= 'a' && key.character <= 'z') ||
             (key.character >= 'A' && key.character <= 'Z')))
            in->shifted = 0;
        return result;
    }
    case CN_ACCOUNT_TEXT_KEY_SHIFT:
        cn_account_text_toggle_shift(in);
        return CN_ACCOUNT_TEXT_CHANGED;
    case CN_ACCOUNT_TEXT_KEY_SYMBOLS:
        cn_account_text_toggle_symbols(in);
        return CN_ACCOUNT_TEXT_CHANGED;
    case CN_ACCOUNT_TEXT_KEY_BACKSPACE:
        return cn_account_text_erase(in);
    case CN_ACCOUNT_TEXT_KEY_CURSOR_LEFT:
        return cn_account_text_move_cursor(in, -1) ?
               CN_ACCOUNT_TEXT_CHANGED : CN_ACCOUNT_TEXT_UNCHANGED;
    case CN_ACCOUNT_TEXT_KEY_CURSOR_RIGHT:
        return cn_account_text_move_cursor(in, 1) ?
               CN_ACCOUNT_TEXT_CHANGED : CN_ACCOUNT_TEXT_UNCHANGED;
    case CN_ACCOUNT_TEXT_KEY_DONE:
        return CN_ACCOUNT_TEXT_DONE;
    default:
        return CN_ACCOUNT_TEXT_UNCHANGED;
    }
}

size_t cn_account_text_window_start(const cn_account_text_input *in,
                                    size_t visible)
{
    size_t start;
    if (!in || in->length <= visible) return 0;
    start = in->cursor > visible ? in->cursor - visible : 0;
    if (start + visible > in->length) start = in->length - visible;
    return start;
}
