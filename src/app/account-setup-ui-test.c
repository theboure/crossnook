#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "account/account_setup_controller.h"
#include "app/account_setup_ui.h"
#include "graphics/canvas.h"
#include "graphics/text.h"
#include "ui/account_text_input.h"
#include "ui/ui.h"

static int failures;
#define CHECK(c, name) do { if (!(c)) { printf("[FAIL] %s\n", name); ++failures; } \
                           else printf("[OK] %s\n", name); } while (0)

#define ACCOUNT_TEST_KEY_X 45
#define ACCOUNT_TEST_KEY_Y 540
#define ACCOUNT_TEST_LEFT_X 190
#define ACCOUNT_TEST_DONE_X 544
#define ACCOUNT_TEST_LAST_ROW_Y 714
#define ACCOUNT_TEST_SUBMIT_X 300
#define ACCOUNT_TEST_SUBMIT_Y 427
#define ACCOUNT_TEST_ACTION_Y 220
#define ACCOUNT_TEST_SINGLE_ACTION_Y 631
#define ACCOUNT_TEST_GRID_X 20
#define ACCOUNT_TEST_GRID_Y 474
#define ACCOUNT_TEST_GRID_STEP_X 56
#define ACCOUNT_TEST_GRID_STEP_Y 54
#define ACCOUNT_TEST_GRID_WIDTH 51
#define ACCOUNT_TEST_PASSWORD_TOP (126 + 3 * 66)

static void event(cn_ui *ui, cn_input_event type, int x, int y)
{
    cn_input_ev ev;
    memset(&ev, 0, sizeof ev); ev.type = type; ev.x = x; ev.y = y;
    (void)cn_ui_handle(ui, &ev);
}

static int password_value_matches(cn_canvas *actual, cn_canvas *expected,
                                  cn_text *text, const char *mask)
{
    int ascent, baseline, x, y;
    cn_canvas_clear(expected, CN_COLOR_WHITE);
    ascent = cn_text_set_size(text, 22);
    baseline = ACCOUNT_TEST_PASSWORD_TOP + 24 + (39 - 22) / 2 +
               (ascent > 0 ? ascent : 0);
    if (mask && *mask)
        (void)cn_text_render(text, expected, mask, &baseline, 44, 32,
                             CN_COLOR_BLACK, CN_COLOR_WHITE);
    for (y = ACCOUNT_TEST_PASSWORD_TOP + 24;
         y <= ACCOUNT_TEST_PASSWORD_TOP + 62; ++y)
        for (x = 44; x <= 567; ++x)
            if (cn_canvas_get_pixel(actual, x, y) !=
                cn_canvas_get_pixel(expected, x, y))
                return 0;
    return 1;
}

static int check_character_row(const cn_account_text_input *in, int row,
                               const char *characters)
{
    int column;
    size_t length = strlen(characters);
    for (column = 0; column < CN_ACCOUNT_TEXT_KEY_COLUMNS; ++column) {
        cn_account_text_key key;
        int present = cn_account_text_key_at(in, row, column, &key);
        if ((size_t)column < length) {
            if (!present || key.kind != CN_ACCOUNT_TEXT_KEY_CHARACTER ||
                key.character != (unsigned char)characters[column])
                return 0;
        } else if (present) return 0;
    }
    return 1;
}

static int check_control(const cn_account_text_input *in, int row, int column,
                         cn_account_text_key_kind kind, const char *label,
                         unsigned char character)
{
    cn_account_text_key key;
    if (!cn_account_text_key_at(in, row, column, &key) || key.kind != kind)
        return 0;
    if (kind == CN_ACCOUNT_TEXT_KEY_CHARACTER)
        return key.character == character &&
               ((!label && !key.label) || (label && key.label &&
                                            !strcmp(label, key.label)));
    return key.label && label && !strcmp(key.label, label);
}

static int check_keyboard_descriptors(void)
{
    static const char *const lower[3] = {
        "1234567890", "qwertyuiop", "asdfghjkl"
    };
    static const char *const upper[3] = {
        "1234567890", "QWERTYUIOP", "ASDFGHJKL"
    };
    static const char *const symbols[3] = {
        "1234567890", "+-=^_@#$%&", "!?:;'\"()*`"
    };
    char bytes[64];
    unsigned char covered[128];
    cn_account_text_input in;
    int row, column, c, ok = 1;
    memset(covered, 0, sizeof covered);
    cn_account_text_init(&in, bytes, sizeof bytes);

    for (row = 0; row < 3; ++row)
        ok &= check_character_row(&in, row, lower[row]);
    ok &= check_control(&in, 3, 0, CN_ACCOUNT_TEXT_KEY_SHIFT, "SHIFT", 0);
    for (column = 1; column <= 7; ++column)
        ok &= check_control(&in, 3, column, CN_ACCOUNT_TEXT_KEY_CHARACTER,
                            NULL, (unsigned char)"zxcvbnm"[column - 1]);
    ok &= check_control(&in, 3, 8, CN_ACCOUNT_TEXT_KEY_BACKSPACE, "DEL", 0);
    ok &= !cn_account_text_key_at(&in, 3, 9, &(cn_account_text_key){0});
    ok &= check_control(&in, 4, 0, CN_ACCOUNT_TEXT_KEY_SYMBOLS, "#+=", 0);
    ok &= check_control(&in, 4, 1, CN_ACCOUNT_TEXT_KEY_CHARACTER, NULL, '/');
    ok &= check_control(&in, 4, 2, CN_ACCOUNT_TEXT_KEY_CURSOR_LEFT,
                        "\xe2\x86\x90", 0);
    ok &= check_control(&in, 4, 3, CN_ACCOUNT_TEXT_KEY_CHARACTER, "SPACE", ' ');
    ok &= check_control(&in, 4, 4, CN_ACCOUNT_TEXT_KEY_CURSOR_RIGHT,
                        "\xe2\x86\x92", 0);
    ok &= check_control(&in, 4, 5, CN_ACCOUNT_TEXT_KEY_CHARACTER, NULL, '.');
    ok &= check_control(&in, 4, 6, CN_ACCOUNT_TEXT_KEY_DONE, "DONE", 0);
    for (column = 7; column < CN_ACCOUNT_TEXT_KEY_COLUMNS; ++column)
        ok &= !cn_account_text_key_at(&in, 4, column,
                                      &(cn_account_text_key){0});

    cn_account_text_toggle_shift(&in);
    for (row = 0; row < 3; ++row)
        ok &= check_character_row(&in, row, upper[row]);
    for (column = 1; column <= 7; ++column)
        ok &= check_control(&in, 3, column, CN_ACCOUNT_TEXT_KEY_CHARACTER,
                            NULL, (unsigned char)"ZXCVBNM"[column - 1]);

    cn_account_text_toggle_symbols(&in);
    for (row = 0; row < 3; ++row)
        ok &= check_character_row(&in, row, symbols[row]);
    for (column = 0; column <= 8; ++column)
        ok &= check_control(&in, 3, column, CN_ACCOUNT_TEXT_KEY_CHARACTER,
                            NULL, (unsigned char)"[]{}<>\\|~"[column]);
    ok &= check_control(&in, 3, 9, CN_ACCOUNT_TEXT_KEY_BACKSPACE, "DEL", 0);
    ok &= check_control(&in, 4, 0, CN_ACCOUNT_TEXT_KEY_SYMBOLS, "ABC", 0);
    ok &= check_control(&in, 4, 1, CN_ACCOUNT_TEXT_KEY_CHARACTER, NULL, '/');
    ok &= check_control(&in, 4, 2, CN_ACCOUNT_TEXT_KEY_CURSOR_LEFT,
                        "\xe2\x86\x90", 0);
    ok &= check_control(&in, 4, 3, CN_ACCOUNT_TEXT_KEY_CHARACTER, "SPACE", ' ');
    ok &= check_control(&in, 4, 4, CN_ACCOUNT_TEXT_KEY_CURSOR_RIGHT,
                        "\xe2\x86\x92", 0);
    ok &= check_control(&in, 4, 5, CN_ACCOUNT_TEXT_KEY_CHARACTER, NULL, ',');
    ok &= check_control(&in, 4, 6, CN_ACCOUNT_TEXT_KEY_DONE, "DONE", 0);
    for (column = 7; column < CN_ACCOUNT_TEXT_KEY_COLUMNS; ++column)
        ok &= !cn_account_text_key_at(&in, 4, column,
                                      &(cn_account_text_key){0});

    for (c = 0; c < 3; ++c) {
        cn_account_text_input layer;
        cn_account_text_init(&layer, bytes, sizeof bytes);
        if (c == 1) cn_account_text_toggle_shift(&layer);
        if (c == 2) cn_account_text_toggle_symbols(&layer);
        for (row = 0; row < CN_ACCOUNT_TEXT_KEY_ROWS; ++row) {
            for (column = 0; column < CN_ACCOUNT_TEXT_KEY_COLUMNS; ++column) {
                cn_account_text_key key;
                if (cn_account_text_key_at(&layer, row, column, &key) &&
                    key.kind == CN_ACCOUNT_TEXT_KEY_CHARACTER)
                    covered[key.character] = 1;
            }
        }
    }
    for (c = 0x20; c <= 0x7e; ++c)
        if (!covered[c]) ok = 0;
    return ok;
}

static void test_text_cursor(void)
{
    char bytes[8];
    char password[8];
    cn_account_text_input in;
    cn_account_text_key key;

    cn_account_text_init(&in, bytes, sizeof bytes);
    memcpy(bytes, "ac", 3); in.length = 2; in.cursor = 1;
    CHECK(cn_account_text_append(&in, 'b') == CN_ACCOUNT_TEXT_CHANGED &&
          !strcmp(bytes, "abc") && in.cursor == 2,
          "middle insertion shifts the suffix and advances cursor");
    CHECK(cn_account_text_set_cursor(&in, 0) &&
          cn_account_text_append(&in, '0') == CN_ACCOUNT_TEXT_CHANGED &&
          !strcmp(bytes, "0abc") && in.cursor == 1,
          "beginning insertion remains NUL-terminated");
    CHECK(cn_account_text_set_cursor(&in, in.length) &&
          cn_account_text_append(&in, 'd') == CN_ACCOUNT_TEXT_CHANGED &&
          !strcmp(bytes, "0abcd") && bytes[in.length] == 0,
          "end insertion remains NUL-terminated");
    CHECK(!cn_account_text_move_cursor(&in, 1) && in.cursor == in.length,
          "cursor stops at right bound");
    while (cn_account_text_move_cursor(&in, -1)) {}
    CHECK(in.cursor == 0 && !cn_account_text_move_cursor(&in, -1),
          "cursor stops at left bound");
    (void)cn_account_text_set_cursor(&in, 3);
    CHECK(cn_account_text_erase(&in) == CN_ACCOUNT_TEXT_CHANGED &&
          !strcmp(bytes, "0acd") && in.cursor == 2,
          "middle backspace removes the byte before cursor");
    memcpy(bytes, "1234567", 8); in.length = 7; in.cursor = 3;
    CHECK(cn_account_text_append(&in, 'x') == CN_ACCOUNT_TEXT_FULL &&
          !strcmp(bytes, "1234567") && in.cursor == 3 && bytes[7] == 0,
          "full middle insertion preserves buffer and terminator");

    cn_account_text_init(&in, bytes, sizeof bytes);
    (void)cn_account_text_select(&in, 3, 0);
    CHECK(cn_account_text_activate(&in) == CN_ACCOUNT_TEXT_CHANGED && in.shifted &&
          cn_account_text_key_at(&in, 1, 0, &key) && key.character == 'Q',
          "one-shot Shift immediately exposes uppercase labels");
    (void)cn_account_text_select(&in, 0, 0);
    CHECK(cn_account_text_activate(&in) == CN_ACCOUNT_TEXT_CHANGED && in.shifted,
          "non-letter insertion preserves pending Shift");
    (void)cn_account_text_select(&in, 1, 0);
    CHECK(cn_account_text_activate(&in) == CN_ACCOUNT_TEXT_CHANGED && !in.shifted &&
          !strcmp(bytes, "1Q") && cn_account_text_key_at(&in, 1, 0, &key) &&
          key.character == 'q',
          "next alphabetic insertion consumes one-shot Shift");
    (void)cn_account_text_select(&in, 4, 0);
    CHECK(cn_account_text_activate(&in) == CN_ACCOUNT_TEXT_CHANGED && in.symbols,
          "letter layer switches to the single symbol layer");
    (void)cn_account_text_select(&in, 4, 0);
    CHECK(cn_account_text_activate(&in) == CN_ACCOUNT_TEXT_CHANGED && !in.symbols,
          "ABC returns directly to letter layer with no second symbol page");

    cn_account_text_init(&in, password, sizeof password);
    memcpy(password, "abc", 4); in.length = in.cursor = 3;
    cn_account_text_set_cursor_enabled(&in, 0);
    CHECK(!cn_account_text_set_cursor(&in, 0) &&
          !cn_account_text_move_cursor(&in, -1) && in.cursor == 3 &&
          !cn_account_text_key_at(&in, 4, 2, &key) &&
          !cn_account_text_key_at(&in, 4, 4, &key),
          "password input disables cursor movement keys");
    in.cursor = 0;
    CHECK(cn_account_text_append(&in, 'd') == CN_ACCOUNT_TEXT_CHANGED &&
          !strcmp(password, "abcd") && in.cursor == 4 &&
          cn_account_text_erase(&in) == CN_ACCOUNT_TEXT_CHANGED &&
          !strcmp(password, "abc") && in.cursor == 3,
          "password insertion and backspace remain end-only");
}

static void test_keyboard_focus(const char *font_path)
{
    cn_ui *ui = cn_ui_init();
    cn_canvas *canvas = cn_canvas_create(600, 800);
    cn_text *text = cn_text_load(font_path);
    uint16_t *first = NULL;
    int visible = 0, touch_returns_to_form = 0, field_geometry = 0;
    int back_to_home = 0;
    if (!ui || !canvas || !text) {
        CHECK(0, "keyboard focus render fixtures");
        cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
        return;
    }
    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_PRECONDITION,
                             CN_UI_ACCOUNT_LOCAL_NO_ACCOUNT);
    event(ui, CN_INPUT_MENU, 0, 0);
    cn_ui_render(ui, canvas, text);
    first = malloc(600u * 800u * sizeof *first);
    if (first) memcpy(first, cn_canvas_pixels(canvas),
                      600u * 800u * sizeof *first);
    event(ui, CN_INPUT_PAGE_NEXT, 0, 0);
    cn_ui_render(ui, canvas, text);
    visible = first && memcmp(first, cn_canvas_pixels(canvas),
                              600u * 800u * sizeof *first) != 0;
    event(ui, CN_INPUT_BACK, 0, 0);
    cn_ui_render(ui, canvas, text);
    touch_returns_to_form = cn_ui_get_state(ui) == CN_UI_ACCOUNT_SETUP &&
        (!first || memcmp(first, cn_canvas_pixels(canvas),
                           600u * 800u * sizeof *first) != 0);
    if (first) {
        memcpy(first, cn_canvas_pixels(canvas), 600u * 800u * sizeof *first);
        event(ui, CN_INPUT_TOUCH_UP, 10, 220);
        cn_ui_render(ui, canvas, text);
        field_geometry = !memcmp(first, cn_canvas_pixels(canvas),
                                 600u * 800u * sizeof *first);
        event(ui, CN_INPUT_TOUCH_UP, 100, 220);
        cn_ui_render(ui, canvas, text);
        field_geometry = field_geometry &&
            memcmp(first, cn_canvas_pixels(canvas),
                   600u * 800u * sizeof *first) != 0;
    }
    event(ui, CN_INPUT_MENU, 0, 0);
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y);
    event(ui, CN_INPUT_BACK, 0, 0);
    back_to_home = cn_ui_get_state(ui) == CN_UI_HOME;
    CHECK(visible, "physical NEXT changes visible keyboard selection pixels");
    CHECK(touch_returns_to_form,
          "keyboard BACK returns to visibly unselected form mode");
    CHECK(field_geometry,
          "field focus hit geometry matches the visible content width");
    CHECK(back_to_home,
          "touch interaction exits keyboard mode before form BACK leaves");
    free(first); cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
}

static void test_cursor_keyboard_ui(const char *font_path)
{
    static const char original[] = "https://very.long.synthetic.invalid/path";
    cn_ui *ui = cn_ui_init();
    cn_canvas *canvas = cn_canvas_create(600, 800);
    cn_text *text = cn_text_load(font_path);
    cn_ui_account_input input;
    uint16_t *frame = NULL;
    size_t i;
    int geometry_ok = 0, caret_moved = 0, shift_visible = 0;
    int symbol_visible = 0, tap_inserted = 0, done_exited = 0;
    if (!ui || !canvas || !text) {
        CHECK(0, "cursor/keyboard render fixtures");
        cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
        return;
    }
    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_PRECONDITION,
                             CN_UI_ACCOUNT_LOCAL_NO_ACCOUNT);
    cn_ui_account_prefill(ui, original, "reader");
    cn_ui_render(ui, canvas, text);
    frame = malloc(600u * 800u * sizeof *frame);
    if (frame) memcpy(frame, cn_canvas_pixels(canvas),
                      600u * 800u * sizeof *frame);

    geometry_ok = cn_canvas_get_pixel(canvas, 20, 528) == CN_COLOR_BLACK &&
                  cn_canvas_get_pixel(canvas, 71, 576) == CN_COLOR_BLACK &&
                  cn_canvas_get_pixel(canvas, 73, 550) == CN_COLOR_WHITE;
    event(ui, CN_INPUT_TOUCH_UP, 73, 550); /* gap between q and w */
    (void)cn_ui_account_get_input(ui, &input);
    geometry_ok = geometry_ok && !strcmp(input.base_url, original);
    CHECK(geometry_ok,
          "visible key bounds and touch gaps share canonical geometry");

    for (i = 3; i < strlen(original); ++i)
        event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_LEFT_X,
              ACCOUNT_TEST_LAST_ROW_Y);
    cn_ui_render(ui, canvas, text);
    caret_moved = frame && memcmp(frame, cn_canvas_pixels(canvas),
                                  600u * 800u * sizeof *frame) != 0;
    CHECK(caret_moved,
          "long URL window follows the static caret near the beginning");

    event(ui, CN_INPUT_TOUCH_UP, 44, 170); /* first visible caret stop */
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y);
    (void)cn_ui_account_get_input(ui, &input);
    tap_inserted = input.base_url[0] == 'q' &&
                   !strcmp(input.base_url + 1, original);
    CHECK(tap_inserted,
          "value touch places cursor and inserts near long URL beginning");

    cn_ui_render(ui, canvas, text);
    if (frame) memcpy(frame, cn_canvas_pixels(canvas),
                      600u * 800u * sizeof *frame);
    event(ui, CN_INPUT_TOUCH_UP, 55, 660); /* SHIFT */
    cn_ui_render(ui, canvas, text);
    shift_visible = frame && memcmp(frame, cn_canvas_pixels(canvas),
                                    600u * 800u * sizeof *frame) != 0;
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y);
    (void)cn_ui_account_get_input(ui, &input);
    shift_visible = shift_visible && input.base_url[1] == 'Q';
    CHECK(shift_visible,
          "pending Shift visibly redraws uppercase and clears after letter");

    cn_ui_render(ui, canvas, text);
    if (frame) memcpy(frame, cn_canvas_pixels(canvas),
                      600u * 800u * sizeof *frame);
    event(ui, CN_INPUT_TOUCH_UP, 55, ACCOUNT_TEST_LAST_ROW_Y); /* #+= */
    cn_ui_render(ui, canvas, text);
    symbol_visible = frame && memcmp(frame, cn_canvas_pixels(canvas),
                                     600u * 800u * sizeof *frame) != 0;
    event(ui, CN_INPUT_TOUCH_UP, 55, ACCOUNT_TEST_LAST_ROW_Y); /* ABC */
    CHECK(symbol_visible,
          "symbol mode visibly replaces the complete canonical key set");

    event(ui, CN_INPUT_MENU, 0, 0); /* physical keyboard selection mode */
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_DONE_X,
          ACCOUNT_TEST_LAST_ROW_Y);
    done_exited = cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE;
    event(ui, CN_INPUT_BACK, 0, 0);
    done_exited = done_exited && cn_ui_get_state(ui) == CN_UI_HOME;
    CHECK(done_exited, "DONE exits keyboard mode without submitting form");

    free(frame); cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
}

static void test_symbol_keyboard_geometry(const char *font_path)
{
    static const char *const rows[4] = {
        "1234567890", "+-=^_@#$%&", "!?:;'\"()*`", "[]{}<>\\|~"
    };
    static const int utility_centers[7] = { 55, 123, 192, 299, 406, 475, 543 };
    cn_ui *ui = cn_ui_init();
    cn_canvas *canvas = cn_canvas_create(600, 800);
    cn_text *text = cn_text_load(font_path);
    cn_ui_account_input input;
    char expected[128];
    size_t length = 0;
    int row, column;
    int letter_stagger = 1, symbol_grid = 1, gaps_clear = 1;
    int centers_exact = 1, utility_exact = 1, done_exact = 0;
    if (!ui || !canvas || !text) {
        CHECK(0, "symbol keyboard geometry fixtures");
        cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
        return;
    }

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_PRECONDITION,
                             CN_UI_ACCOUNT_LOCAL_NO_ACCOUNT);
    cn_ui_render(ui, canvas, text);
    letter_stagger =
        cn_canvas_get_pixel(canvas, 20, ACCOUNT_TEST_GRID_Y +
                            2 * ACCOUNT_TEST_GRID_STEP_Y + 2) == CN_COLOR_WHITE &&
        cn_canvas_get_pixel(canvas, 48, ACCOUNT_TEST_GRID_Y +
                            2 * ACCOUNT_TEST_GRID_STEP_Y + 2) == CN_COLOR_BLACK &&
        cn_canvas_get_pixel(canvas, 92, ACCOUNT_TEST_GRID_Y +
                            3 * ACCOUNT_TEST_GRID_STEP_Y + 2) == CN_COLOR_WHITE &&
        cn_canvas_get_pixel(canvas, 98, ACCOUNT_TEST_GRID_Y +
                            3 * ACCOUNT_TEST_GRID_STEP_Y + 2) == CN_COLOR_BLACK;
    CHECK(letter_stagger, "letter rows retain accepted QWERTY staggering");

    event(ui, CN_INPUT_TOUCH_UP, utility_centers[0],
          ACCOUNT_TEST_LAST_ROW_Y);
    cn_ui_render(ui, canvas, text);
    for (row = 0; row < 4; ++row) {
        int y = ACCOUNT_TEST_GRID_Y + row * ACCOUNT_TEST_GRID_STEP_Y + 2;
        for (column = 0; column < 10; ++column) {
            int x = ACCOUNT_TEST_GRID_X + column * ACCOUNT_TEST_GRID_STEP_X;
            symbol_grid = symbol_grid &&
                cn_canvas_get_pixel(canvas, x, y) == CN_COLOR_BLACK &&
                cn_canvas_get_pixel(canvas, x + ACCOUNT_TEST_GRID_WIDTH, y) ==
                    CN_COLOR_BLACK;
            if (column < 9)
                symbol_grid = symbol_grid &&
                    cn_canvas_get_pixel(canvas, x + ACCOUNT_TEST_GRID_WIDTH + 2,
                                        y) == CN_COLOR_WHITE;
        }
    }
    CHECK(symbol_grid,
          "symbol rows 1-4 share one aligned non-QWERTY 10-column grid");

    for (row = 0; row < 4; ++row) {
        int y = ACCOUNT_TEST_GRID_Y + row * ACCOUNT_TEST_GRID_STEP_Y + 24;
        for (column = 0; column < 9; ++column) {
            int x = ACCOUNT_TEST_GRID_X + column * ACCOUNT_TEST_GRID_STEP_X +
                    ACCOUNT_TEST_GRID_WIDTH + 2;
            event(ui, CN_INPUT_TOUCH_UP, x, y);
        }
    }
    (void)cn_ui_account_get_input(ui, &input);
    gaps_clear = input.base_url[0] == 0;
    CHECK(gaps_clear, "symbol-row gap touches activate nothing");

    for (row = 0; row < 4; ++row) {
        for (column = 0; rows[row][column]; ++column) {
            int x = ACCOUNT_TEST_GRID_X + column * ACCOUNT_TEST_GRID_STEP_X +
                    ACCOUNT_TEST_GRID_WIDTH / 2;
            int y = ACCOUNT_TEST_GRID_Y + row * ACCOUNT_TEST_GRID_STEP_Y + 24;
            event(ui, CN_INPUT_TOUCH_UP, x, y);
            expected[length++] = rows[row][column];
        }
    }
    expected[length] = 0;
    (void)cn_ui_account_get_input(ui, &input);
    centers_exact = !strcmp(input.base_url, expected);

    event(ui, CN_INPUT_TOUCH_UP,
          ACCOUNT_TEST_GRID_X + 9 * ACCOUNT_TEST_GRID_STEP_X +
          ACCOUNT_TEST_GRID_WIDTH / 2,
          ACCOUNT_TEST_GRID_Y + 3 * ACCOUNT_TEST_GRID_STEP_Y + 24);
    expected[--length] = 0;
    (void)cn_ui_account_get_input(ui, &input);
    centers_exact = centers_exact && !strcmp(input.base_url, expected);
    CHECK(centers_exact,
          "every symbol-grid key center activates its exact character or DEL");

    event(ui, CN_INPUT_TOUCH_UP, utility_centers[1], ACCOUNT_TEST_LAST_ROW_Y);
    expected[length++] = '/';
    event(ui, CN_INPUT_TOUCH_UP, utility_centers[2], ACCOUNT_TEST_LAST_ROW_Y);
    event(ui, CN_INPUT_TOUCH_UP, utility_centers[5], ACCOUNT_TEST_LAST_ROW_Y);
    expected[length] = expected[length - 1];
    expected[length - 1] = ',';
    ++length;
    event(ui, CN_INPUT_TOUCH_UP, utility_centers[4], ACCOUNT_TEST_LAST_ROW_Y);
    event(ui, CN_INPUT_TOUCH_UP, utility_centers[3], ACCOUNT_TEST_LAST_ROW_Y);
    expected[length++] = ' ';
    expected[length] = 0;
    (void)cn_ui_account_get_input(ui, &input);
    utility_exact = !strcmp(input.base_url, expected);

    event(ui, CN_INPUT_TOUCH_UP, utility_centers[0], ACCOUNT_TEST_LAST_ROW_Y);
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y);
    expected[length++] = 'q';
    expected[length] = 0;
    (void)cn_ui_account_get_input(ui, &input);
    utility_exact = utility_exact && !strcmp(input.base_url, expected);
    CHECK(utility_exact,
          "every symbol utility-key center preserves its exact action");

    event(ui, CN_INPUT_TOUCH_UP, utility_centers[0], ACCOUNT_TEST_LAST_ROW_Y);
    event(ui, CN_INPUT_MENU, 0, 0);
    event(ui, CN_INPUT_TOUCH_UP, utility_centers[6], ACCOUNT_TEST_LAST_ROW_Y);
    (void)cn_ui_account_get_input(ui, &input);
    event(ui, CN_INPUT_BACK, 0, 0);
    done_exact = !strcmp(input.base_url, expected) &&
                 cn_ui_get_state(ui) == CN_UI_HOME &&
                 cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE;
    CHECK(done_exact, "symbol DONE center exits keyboard mode without mutation");

    cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
}

static void test_action_hit_targets(const char *font_path)
{
    cn_ui *ui = cn_ui_init();
    cn_canvas *canvas = cn_canvas_create(600, 800);
    cn_text *text = cn_text_load(font_path);
    cn_ui_account_input view;
    const unsigned char *password = NULL;
    int wiped = 0;
    if (!ui || !canvas || !text) {
        CHECK(0, "action hit test UI allocation");
        cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
        return;
    }

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_PRECONDITION,
                             CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED);
    event(ui, CN_INPUT_TOUCH_UP, 300, ACCOUNT_TEST_ACTION_Y); /* gap between real buttons */
    event(ui, CN_INPUT_TOUCH_UP, 120, 320); /* outside button row */
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE &&
          cn_ui_get_state(ui) == CN_UI_ACCOUNT_SETUP,
          "touch outside rendered actions does nothing");
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_ACTIVATE_EXISTING,
          "complete-disabled retry is the first explicit action");

    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_AUTH_REJECTED,
                             CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED);
    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_AUTH_REJECTED,
                             CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE &&
          cn_ui_get_state(ui) == CN_UI_ACCOUNT_SETUP,
          "AUTH_REJECTED correction opens form without activation");
    event(ui, CN_INPUT_BACK, 0, 0);
    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_AUTH_REJECTED,
                             CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED);
    CHECK(cn_ui_account_get_input(ui, &view) == 0, "AUTH_REJECTED input view");
    password = view.password;
    ((unsigned char *)view.password)[0] = 'x';
    ((unsigned char *)view.password)[CN_ACCOUNT_PASSWORD_MAX] = 'x';
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y); /* actual Back rectangle */
    wiped = cn_ui_get_state(ui) == CN_UI_HOME && password &&
        password[0] == 0 && password[CN_ACCOUNT_PASSWORD_MAX] == 0;
    CHECK(wiped, "touch Back returns HOME and wipes the password allocation");

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_INFRASTRUCTURE_FAILED,
                             CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED);
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME &&
          cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE,
          "infrastructure Back cannot activate or recheck");

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_ACTIVATION_UNCERTAIN,
                             CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_RECHECK,
          "durability uncertainty exposes read-only Recheck");
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "durability uncertainty Back returns HOME");

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_CORRUPT,
                             CN_UI_ACCOUNT_LOCAL_CORRUPT);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_RECHECK,
          "corrupt local state exposes read-only Recheck");
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "corrupt local state Back returns HOME");

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_INCOMPLETE,
                             CN_UI_ACCOUNT_LOCAL_ORPHAN_CREDENTIALS);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_RECHECK,
          "incomplete local state exposes only read-only Recheck");
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "incomplete local state Back returns HOME");

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_PRECONDITION,
                             CN_UI_ACCOUNT_LOCAL_UNREADABLE);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_RECHECK,
          "unreadable local state exposes read-only Recheck");
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "unreadable local state Back returns HOME");

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_ALREADY_ENABLED,
                             CN_UI_ACCOUNT_LOCAL_ENABLED);
    cn_ui_render(ui, canvas, text);
    CHECK(cn_canvas_get_pixel(canvas, 100, ACCOUNT_TEST_ACTION_Y) ==
              CN_COLOR_WHITE &&
          cn_canvas_get_pixel(canvas, 100, ACCOUNT_TEST_SINGLE_ACTION_Y) ==
              CN_COLOR_BLACK,
          "single Back renders only in the lower canonical action zone");
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y); /* no right-hand button rendered */
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y); /* former Back location */
    CHECK(cn_ui_get_state(ui) == CN_UI_ACCOUNT_SETUP &&
          cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE,
          "former single-Back location has no touch target");
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_SINGLE_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "lower single-Back center returns HOME");

    event(ui, CN_INPUT_MENU, 0, 0);
    (void)cn_ui_account_take_action(ui);
    cn_ui_account_set_result(ui, CN_UI_ACCOUNT_RESULT_PROFILE_UNAVAILABLE,
                             CN_UI_ACCOUNT_LOCAL_ENABLED);
    event(ui, CN_INPUT_TOUCH_UP, 400, ACCOUNT_TEST_ACTION_Y);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_ACCOUNT_SETUP &&
          cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE,
          "profile-unavailable result ignores the former action row");
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_SINGLE_ACTION_Y);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "profile-unavailable uses the same lower Back center");
    cn_ui_free(ui); cn_canvas_free(canvas); cn_text_free(text);
}

static cn_account_setup_report report(cn_account_setup_local_state local,
                                      cn_account_setup_status status)
{
    cn_account_setup_report r;
    memset(&r, 0, sizeof r); r.local_state = local; r.status = status;
    return r;
}

static void set_report(cn_ui *ui, const cn_account_setup_report *r)
{
    cn_ui_account_result status = CN_UI_ACCOUNT_RESULT_OTHER;
    switch (r->status) {
    case CN_ACCOUNT_SETUP_ACTIVATED: status = CN_UI_ACCOUNT_RESULT_ACTIVATED; break;
    case CN_ACCOUNT_SETUP_ALREADY_ENABLED: status = CN_UI_ACCOUNT_RESULT_ALREADY_ENABLED; break;
    case CN_ACCOUNT_SETUP_INVALID_INPUT: status = CN_UI_ACCOUNT_RESULT_INVALID_INPUT; break;
    case CN_ACCOUNT_SETUP_PRECONDITION: status = CN_UI_ACCOUNT_RESULT_PRECONDITION; break;
    case CN_ACCOUNT_SETUP_BOOTSTRAP_FAILED: status = CN_UI_ACCOUNT_RESULT_BOOTSTRAP_FAILED; break;
    case CN_ACCOUNT_SETUP_IDENTITY_FAILED: status = CN_UI_ACCOUNT_RESULT_IDENTITY_FAILED; break;
    case CN_ACCOUNT_SETUP_ACTIVATION_SAVE_FAILED: status = CN_UI_ACCOUNT_RESULT_SAVE_FAILED; break;
    case CN_ACCOUNT_SETUP_ACTIVATION_FAILED: status = CN_UI_ACCOUNT_RESULT_ACTIVATION_FAILED; break;
    case CN_ACCOUNT_SETUP_INCOMPLETE_STATE: status = CN_UI_ACCOUNT_RESULT_INCOMPLETE; break;
    case CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE: status = CN_UI_ACCOUNT_RESULT_CORRUPT; break;
    case CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN: status = CN_UI_ACCOUNT_RESULT_BOOTSTRAP_UNCERTAIN; break;
    case CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN: status = CN_UI_ACCOUNT_RESULT_IDENTITY_UNCERTAIN; break;
    case CN_ACCOUNT_SETUP_AUTH_REJECTED: status = CN_UI_ACCOUNT_RESULT_AUTH_REJECTED; break;
    case CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED: status = CN_UI_ACCOUNT_RESULT_INFRASTRUCTURE_FAILED; break;
    case CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN: status = CN_UI_ACCOUNT_RESULT_ACTIVATION_UNCERTAIN; break;
    case CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE: status = CN_UI_ACCOUNT_RESULT_PROFILE_UNAVAILABLE; break;
    default: break;
    }
    cn_ui_account_set_result(ui, status, (cn_ui_account_local)r->local_state);
}

int main(int argc, char **argv)
{
    char bounded[3];
    char url_bound[CN_SETTINGS_KOSYNC_URL_CAPACITY];
    cn_account_text_input text_input;
    cn_account_text_input url_input;
    cn_ui *ui;
    cn_ui_account_input input;
    cn_account_setup_report r;
    cn_canvas *canvas;
    cn_text *text;
    cn_account_setup_ui app;
    cn_account_setup_ui_config bad_config;
    uint16_t *masked_frame;
    cn_canvas *password_expected;
    int i;
    if (argc != 2) return 2;

    CHECK(check_keyboard_descriptors(),
          "exact letter/symbol layouts preserve printable ASCII coverage");
    test_text_cursor();
    test_keyboard_focus(argv[1]);
    test_cursor_keyboard_ui(argv[1]);
    test_symbol_keyboard_geometry(argv[1]);
    test_action_hit_targets(argv[1]);

    cn_account_text_init(&text_input, bounded, sizeof bounded);
    CHECK(cn_account_text_append(&text_input, 'a') == CN_ACCOUNT_TEXT_CHANGED &&
          cn_account_text_append(&text_input, 'B') == CN_ACCOUNT_TEXT_CHANGED &&
          cn_account_text_append(&text_input, 'c') == CN_ACCOUNT_TEXT_FULL,
          "bounded append refuses truncation");
    cn_account_text_toggle_shift(&text_input);
    cn_account_text_toggle_symbols(&text_input);
    CHECK(cn_account_text_erase(&text_input) == CN_ACCOUNT_TEXT_CHANGED &&
          text_input.length == 1 && cn_account_text_window_start(&text_input, 1) == 0,
          "erase, shift/symbol layers, and visible window");
    cn_account_text_init(&url_input, url_bound, sizeof url_bound);
    for (i = 0; i < CN_SETTINGS_KOSYNC_URL_MAX; ++i)
        if (cn_account_text_append(&url_input, 'a') != CN_ACCOUNT_TEXT_CHANGED)
            break;
    CHECK(url_input.length == CN_SETTINGS_KOSYNC_URL_MAX &&
          cn_account_text_append(&url_input, 'b') == CN_ACCOUNT_TEXT_FULL,
          "URL field obeys published byte limit");

    ui = cn_ui_init();
    canvas = cn_canvas_create(600, 800);
    password_expected = cn_canvas_create(600, 800);
    text = cn_text_load(argv[1]);
    CHECK(ui && canvas && password_expected && text, "600x800 UI allocations");
    if (!ui || !canvas || !password_expected || !text) return 1;

    event(ui, CN_INPUT_TOUCH_UP, 100, 438);
    CHECK(cn_ui_get_state(ui) == CN_UI_ACCOUNT_SETUP &&
          cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_INSPECT,
          "HOME touch enters account and emits inspect");
    r = report(CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT, CN_ACCOUNT_SETUP_PRECONDITION);
    set_report(ui, &r);
    event(ui, CN_INPUT_TOUCH_UP, 200, 160);
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y); /* q */
    cn_ui_render(ui, canvas, text);
    CHECK(cn_canvas_width(canvas) == 600 && cn_canvas_height(canvas) == 800 &&
           cn_text_get_stats(text).ink > 0,
           "account form/keyboard render at 600x800");
    CHECK(cn_canvas_get_pixel(canvas, 34, 150) == CN_COLOR_BLACK &&
          cn_canvas_get_pixel(canvas, 300, 156) == CN_COLOR_WHITE,
          "focused field keeps black marker without gray band fill");
    CHECK(cn_ui_account_get_input(ui, &input) == 0 &&
          strcmp(input.base_url, "q") == 0,
          "touch keyboard character entry");
    cn_ui_account_prefill(ui, "https://setup.synthetic.invalid/base",
                          "Synthetic Reader");
    r = report(CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED,
               CN_ACCOUNT_SETUP_PRECONDITION);
    set_report(ui, &r);
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_SUBMIT_X, ACCOUNT_TEST_SUBMIT_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_SUBMIT_NEW_OR_RESUME,
          "partial account resumes only through NEW_OR_RESUME");
    r = report(CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT, CN_ACCOUNT_SETUP_PRECONDITION);
    set_report(ui, &r);

    event(ui, CN_INPUT_PAGE_NEXT, 0, 0);
    event(ui, CN_INPUT_PAGE_PREV, 0, 0);
    event(ui, CN_INPUT_MENU, 0, 0); /* keyboard mode */
    event(ui, CN_INPUT_PAGE_NEXT, 0, 0);
    event(ui, CN_INPUT_MENU, 0, 0);
    CHECK(cn_ui_account_get_input(ui, &input) == 0,
          "physical field and keyboard navigation");
    event(ui, CN_INPUT_BACK, 0, 0); /* keyboard -> form */
    CHECK(cn_ui_get_state(ui) == CN_UI_ACCOUNT_SETUP,
          "BACK from keyboard returns to form");

    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_SUBMIT_X, ACCOUNT_TEST_SUBMIT_Y); /* submit */
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_SUBMIT_NEW_OR_RESUME,
          "new account submit emits NEW_OR_RESUME");
    cn_ui_account_get_input(ui, &input);
    CHECK(input.password_length == 0, "password remains empty until entered");
    r = report(CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT, CN_ACCOUNT_SETUP_INVALID_INPUT);
    set_report(ui, &r);
    cn_ui_account_get_input(ui, &input);
    CHECK(input.password_length == 0 &&
          strcmp(input.base_url, "https://setup.synthetic.invalid/basew") == 0,
          "invalid input retains non-secret text and clears password");
    event(ui, CN_INPUT_HOME, 0, 0);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "HOME leaves account setup and clears transient fields");

    /* Repeated entry begins with another read-only inspection, never stale input. */
    event(ui, CN_INPUT_MENU, 0, 0);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_INSPECT,
          "re-entry requests inspection only");
    r = report(CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED,
               CN_ACCOUNT_SETUP_PRECONDITION);
    set_report(ui, &r);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_ACTIVATE_EXISTING,
          "complete disabled default retries without credential input");
    r = report(CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED,
               CN_ACCOUNT_SETUP_PRECONDITION);
    set_report(ui, &r);
    event(ui, CN_INPUT_TOUCH_UP, 450, ACCOUNT_TEST_ACTION_Y); /* explicit correction */
    CHECK(cn_ui_account_get_input(ui, &input) == 0 && input.password_length == 0,
          "disabled correction opens blank secret form");
    event(ui, CN_INPUT_TOUCH_UP, 200, 357); /* password field */
    cn_ui_render(ui, canvas, text);
    CHECK(password_value_matches(canvas, password_expected, text, ""),
          "empty password renders no mask and no caret");
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y);  /* one character */
    cn_ui_render(ui, canvas, text);
    CHECK(password_value_matches(canvas, password_expected, text, "*"),
          "one password byte renders exactly one asterisk");
    masked_frame = malloc(600u * 800u * sizeof *masked_frame);
    if (masked_frame)
        memcpy(masked_frame, cn_canvas_pixels(canvas),
                600u * 800u * sizeof *masked_frame);
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_LEFT_X,
          ACCOUNT_TEST_LAST_ROW_Y);
    cn_ui_render(ui, canvas, text);
    cn_ui_account_get_input(ui, &input);
    CHECK(masked_frame && !memcmp(masked_frame, cn_canvas_pixels(canvas),
                                  600u * 800u * sizeof *masked_frame) &&
          input.password_length == 1,
          "password hides cursor position and omits LEFT/RIGHT behavior");
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y);  /* another character */
    cn_ui_render(ui, canvas, text);
    CHECK(password_value_matches(canvas, password_expected, text, "**") &&
          masked_frame && memcmp(masked_frame, cn_canvas_pixels(canvas),
                                 600u * 800u * sizeof *masked_frame),
          "two password bytes render exactly two asterisks");
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_KEY_X, ACCOUNT_TEST_KEY_Y);
    cn_ui_render(ui, canvas, text);
    CHECK(password_value_matches(canvas, password_expected, text, "***"),
          "several password lengths produce matching masks");
    event(ui, CN_INPUT_TOUCH_UP, 530, 660); /* DEL */
    cn_ui_render(ui, canvas, text);
    CHECK(password_value_matches(canvas, password_expected, text, "**"),
          "password backspace reduces the mask by exactly one");
    if (masked_frame)
        memcpy(masked_frame, cn_canvas_pixels(canvas),
               600u * 800u * sizeof *masked_frame);
    event(ui, CN_INPUT_TOUCH_UP, 530, 660); /* DEL */
    event(ui, CN_INPUT_TOUCH_UP, 530, 660); /* DEL */
    event(ui, CN_INPUT_TOUCH_UP, 101, ACCOUNT_TEST_KEY_Y); /* w */
    event(ui, CN_INPUT_TOUCH_UP, 101, ACCOUNT_TEST_KEY_Y); /* w */
    cn_ui_render(ui, canvas, text);
    CHECK(masked_frame && !memcmp(masked_frame, cn_canvas_pixels(canvas),
                                  600u * 800u * sizeof *masked_frame) &&
          password_value_matches(canvas, password_expected, text, "**"),
          "password plaintext never affects rendered pixels; only length does");
    free(masked_frame);
    cn_ui_account_get_input(ui, &input);
    CHECK(input.password_length == 2, "password held in bounded UI field");
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_SUBMIT_X, ACCOUNT_TEST_SUBMIT_Y); /* first submit confirms */
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE,
          "replacement requires explicit confirmation gesture");
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_SUBMIT_X, ACCOUNT_TEST_SUBMIT_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_REPLACE_DISABLED,
          "confirmed correction emits REPLACE_DISABLED");
    cn_ui_account_get_input(ui, &input);
    cn_ui_account_clear_password(ui);
    cn_ui_account_get_input(ui, &input);
    CHECK(input.password_length == 0 && input.password[0] == 0 &&
          input.password[1] == 0,
          "complete password buffer wiped after submission");
    r = report(CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT, CN_ACCOUNT_SETUP_ACTIVATED);
    set_report(ui, &r);
    CHECK(cn_ui_account_local_state(ui) == CN_UI_ACCOUNT_LOCAL_ENABLED,
          "activation success presents enabled local state");
    r = report(CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED,
               CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN);
    set_report(ui, &r);
    event(ui, CN_INPUT_MENU, 0, 0);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_RECHECK,
          "durability uncertainty allows recheck only");
    r = report(CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED,
               CN_ACCOUNT_SETUP_AUTH_REJECTED);
    set_report(ui, &r);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_SUBMIT_X, ACCOUNT_TEST_SUBMIT_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE,
          "rejected auth correction has separate confirmation step");
    event(ui, CN_INPUT_TOUCH_UP, ACCOUNT_TEST_SUBMIT_X, ACCOUNT_TEST_SUBMIT_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_REPLACE_DISABLED,
          "rejected auth correction emits replacement only after confirmation");
    r = report(CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED,
               CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED);
    set_report(ui, &r);
    event(ui, CN_INPUT_TOUCH_UP, 100, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_ACTIVATE_EXISTING,
          "infrastructure result retries activation without input");
    r = report(CN_ACCOUNT_SETUP_LOCAL_ENABLED,
               CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE);
    set_report(ui, &r);
    event(ui, CN_INPUT_TOUCH_UP, 450, ACCOUNT_TEST_ACTION_Y);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE,
          "enabled profile unavailable offers no activation or correction");
    r = report(CN_ACCOUNT_SETUP_LOCAL_CORRUPT_OR_UNSUPPORTED,
               CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE);
    set_report(ui, &r);
    event(ui, CN_INPUT_MENU, 0, 0);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_RECHECK,
          "corrupt local state is read-only fail-closed");
    r = report(CN_ACCOUNT_SETUP_LOCAL_ENABLED,
               CN_ACCOUNT_SETUP_ALREADY_ENABLED);
    set_report(ui, &r);
    cn_ui_render(ui, canvas, text);
    event(ui, CN_INPUT_BACK, 0, 0);
    CHECK(cn_ui_get_state(ui) == CN_UI_HOME,
          "enabled presentation exits without sync action");
    event(ui, CN_INPUT_MENU, 0, 0);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_INSPECT,
          "enabled reopening uses inspection rather than activation");
    r = report(CN_ACCOUNT_SETUP_LOCAL_ENABLED, CN_ACCOUNT_SETUP_ALREADY_ENABLED);
    set_report(ui, &r);
    event(ui, CN_INPUT_MENU, 0, 0);
    CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_NONE,
          "already-enabled presentation has no auth/network intent");

    memset(&bad_config, 0, sizeof bad_config);
    bad_config.mountpoint = "/missing"; bad_config.root = "/missing/root";
    bad_config.dns_ip = "127.0.0.1"; bad_config.sntp_ip = "127.0.0.1";
    bad_config.ca_path = "/missing/ca.pem";
    CHECK(cn_account_setup_ui_init(&app, &bad_config) != 0,
          "storage verification refuses absent mount");
    bad_config.mountpoint = "/mnt/card";
    bad_config.root = "/data/crossnook";
    CHECK(cn_account_setup_ui_init(&app, &bad_config) != 0,
          "coordinator refuses internal /data persistence root");

    /* Make sure the fixed text-entry state stays independent across UI lifetimes. */
    for (i = 0; i < 3; ++i) {
        event(ui, CN_INPUT_HOME, 0, 0);
        event(ui, CN_INPUT_MENU, 0, 0);
        CHECK(cn_ui_account_take_action(ui) == CN_UI_ACCOUNT_INSPECT,
              "repeated entry never skips read-only inspection");
        r = report(CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT, CN_ACCOUNT_SETUP_PRECONDITION);
        set_report(ui, &r);
        cn_ui_account_get_input(ui, &input);
        CHECK(input.password_length == 0,
              "repeated entry has no retained secret bytes");
    }

    cn_ui_free(ui); cn_canvas_free(canvas); cn_canvas_free(password_expected);
    cn_text_free(text);
    printf("ACCOUNT SETUP UI TEST failures=%d\n", failures);
    return failures ? 1 : 0;
}
