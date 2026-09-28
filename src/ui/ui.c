/*
 * ui.c — reusable UI state layer (see ui.h).
 *
 * Layout conventions are the hardware-validated ones from the text
 * milestone: 32 px left/right margins, 16 px top margin, wraps after the
 * last qualifying space, line advance = 11/10 of line height. Everything
 * is drawn with canvas primitives + the text module.
 *
 * Input semantics kept from UI Core: page/menu/back/home buttons with the
 * semantic mapping, touches are committed on TOUCH_UP with the resolved
 * (last in-contact) coordinate, POWER >= 2000 ms requests exit.
 *
 * Library screen applies the following deterministic rules:
 *   - PAGE_NEXT moves the selection down one row; when the selection
 *     crosses the bottom of the viewport the viewport jumps forward so
 *     the selected row becomes its first row (clamped to the list end).
 *   - PAGE_PREV moves the selection up one row and scrolls the viewport
 *     so the selection stays visible.
 *   - Touch on a row selects it; touching the already-selected row again
 *     activates it. EPUB rows open the real reader (CN_UI_READER, page 0)
 *     when a reader is attached; a failing EPUB open and every FB2/TXT
 *     activation show the SELECTED_BOOK diagnostic screen (deterministic
 *     fallback, never a blank or crashed screen).
 *   - CN_UI_READER: NEXT/PREV turn CREngine pages (clamped at first/last),
 *     BACK returns to the library preserving selection/viewport, HOME
 *     returns HOME, POWER >= 2000 ms exits. Touches are ignored (no
 *     diagnostic marker in the real reader).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui/ui.h"
#include "ui/account_text_input.h"
#include "account/kosync_userkey.h"
#include "settings/settings_store.h"
#include "credentials/credential_store.h"

#define CN_ACCOUNT_SETUP_ACTIVATED CN_UI_ACCOUNT_RESULT_ACTIVATED
#define CN_ACCOUNT_SETUP_ALREADY_ENABLED CN_UI_ACCOUNT_RESULT_ALREADY_ENABLED
#define CN_ACCOUNT_SETUP_INVALID_INPUT CN_UI_ACCOUNT_RESULT_INVALID_INPUT
#define CN_ACCOUNT_SETUP_PRECONDITION CN_UI_ACCOUNT_RESULT_PRECONDITION
#define CN_ACCOUNT_SETUP_INCOMPLETE_STATE CN_UI_ACCOUNT_RESULT_INCOMPLETE
#define CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE CN_UI_ACCOUNT_RESULT_CORRUPT
#define CN_ACCOUNT_SETUP_BOOTSTRAP_FAILED CN_UI_ACCOUNT_RESULT_BOOTSTRAP_FAILED
#define CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN CN_UI_ACCOUNT_RESULT_BOOTSTRAP_UNCERTAIN
#define CN_ACCOUNT_SETUP_IDENTITY_FAILED CN_UI_ACCOUNT_RESULT_IDENTITY_FAILED
#define CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN CN_UI_ACCOUNT_RESULT_IDENTITY_UNCERTAIN
#define CN_ACCOUNT_SETUP_AUTH_REJECTED CN_UI_ACCOUNT_RESULT_AUTH_REJECTED
#define CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED CN_UI_ACCOUNT_RESULT_INFRASTRUCTURE_FAILED
#define CN_ACCOUNT_SETUP_ACTIVATION_SAVE_FAILED CN_UI_ACCOUNT_RESULT_SAVE_FAILED
#define CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN CN_UI_ACCOUNT_RESULT_ACTIVATION_UNCERTAIN
#define CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE CN_UI_ACCOUNT_RESULT_PROFILE_UNAVAILABLE
#define CN_ACCOUNT_SETUP_ACTIVATION_FAILED CN_UI_ACCOUNT_RESULT_ACTIVATION_FAILED
#define CN_ACCOUNT_SETUP_LOCAL_UNKNOWN CN_UI_ACCOUNT_LOCAL_UNKNOWN
#define CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT CN_UI_ACCOUNT_LOCAL_NO_ACCOUNT
#define CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED CN_UI_ACCOUNT_LOCAL_PARTIAL_DISABLED
#define CN_ACCOUNT_SETUP_LOCAL_ORPHAN_CREDENTIALS CN_UI_ACCOUNT_LOCAL_ORPHAN_CREDENTIALS
#define CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED
#define CN_ACCOUNT_SETUP_LOCAL_ENABLED CN_UI_ACCOUNT_LOCAL_ENABLED
#define CN_ACCOUNT_SETUP_LOCAL_CORRUPT_OR_UNSUPPORTED CN_UI_ACCOUNT_LOCAL_CORRUPT
#define CN_ACCOUNT_SETUP_LOCAL_UNREADABLE CN_UI_ACCOUNT_LOCAL_UNREADABLE

#define MARGIN_L    32
#define MARGIN_R    32
#define MARGIN_TOP  16

/* "[ Open reader test ]" button geometry (HOME). */
#define BTN_X0      48
#define BTN_X1      340
#define BTN_R_Y0    210
#define BTN_R_Y1    252

/* "[ Open library ]" button geometry (HOME, shown when a library is set). */
#define BTN_L_Y0    300
#define BTN_L_Y1    342

#define MARK_HALF   6   /* marker box half-size */

#define MAX_LIB_CP  160 /* max codepoints considered per library row */

#define SYNC_ROW_TOP 278
#define SYNC_ROW_H 64

#define ACCOUNT_FIELD_X0 32
#define ACCOUNT_FIELD_X1 567
#define ACCOUNT_FIELD_TOP 126
#define ACCOUNT_FIELD_H 66
#define ACCOUNT_SUBMIT_X0 72
#define ACCOUNT_SUBMIT_X1 527
#define ACCOUNT_SUBMIT_Y0 398
#define ACCOUNT_SUBMIT_Y1 456
#define ACCOUNT_ACTION_Y0 190
#define ACCOUNT_ACTION_Y1 252
#define ACCOUNT_SINGLE_ACTION_Y0 600
#define ACCOUNT_SINGLE_ACTION_Y1 662
#define ACCOUNT_SINGLE_ACTION_FOOTER_Y 746
#define ACCOUNT_KEYBOARD_TOP 474
#define ACCOUNT_KEYBOARD_ROW_H 54
#define ACCOUNT_KEY_H 48
#define ACCOUNT_VALUE_STOPS 128

static const char *const account_messages[] = {
    "Account status not checked.", "No account. Enter details and submit.",
    "Setup incomplete. Resume only with saved URL and device name.",
    "Incomplete local account. No automatic repair is available.",
    "Account saved but disabled. Retry activation or explicitly correct.",
    "Sync is enabled.", "Local account is corrupt or unsupported.",
    "Account storage is unreadable.", "Credentials were rejected. Correct explicitly.",
    "Could not reach or use sync service. Check connection and retry.",
    "Storage result uncertain. Recheck local state before proceeding.",
    "Setup failed. Recheck local state.", "Sync enabled, but profile unavailable.",
    "Input is invalid. Correct fields and enter password again.",
    "Working...", "Activation complete. Sync is enabled.",
    "Sync is already enabled.", "Field is full.", "Field input is invalid.",
    "Review correction, then select Continue again to confirm replacement."
};

static const char *CYR_SAMPLE =
    "\xd0\xa1\xd1\x8a\xd0\xb5\xd1\x88\xd1\x8c "
    "\xd0\xb5\xd1\x89\xd1\x91 "
    "\xd1\x8d\xd1\x82\xd0\xb8\xd1\x85 "
    "\xd0\xbc\xd1\x8f\xd0\xb3\xd0\xba\xd0\xb8\xd1\x85 "
    "\xd1\x84\xd1\x80\xd0\xb0\xd0\xbd\xd1\x86\xd1\x83\xd0\xb7"
    "\xd1\x81\xd0\xba\xd0\xb8\xd1\x85 "
    "\xd0\xb1\xd1\x83\xd0\xbb\xd0\xbe\xd0\xba, "
    "\xd0\xb4\xd0\xb0 "
    "\xd0\xb2\xd1\x8b\xd0\xbf\xd0\xb5\xd0\xb9 "
    "\xd1\x87\xd0\xb0\xd1\x8e.";

struct cn_ui {
    cn_ui_state state;
    int         page;

    int         mark_x, mark_y;     /* -1 = no marker yet */

    const cn_library *lib;          /* borrowed, optional */
    int         sel;                /* selected book index */
    int         top;                /* first visible row index */

    cn_reader  *reader;             /* optional EPUB reader (owned) */

    int sync_modal;                  /* 0 none, 1 choices, 2 bounded feedback */
    int sync_selection;              /* 0 local, 1 remote, 2 cancel */
    cn_ui_sync_action sync_action;   /* consumed once by application */
    cn_ui_sync_feedback sync_feedback;

    long long   power_press_ms;     /* -1 = power not held */
    int         exit_requested;

    char account_url[CN_SETTINGS_KOSYNC_URL_CAPACITY];
    char account_device[CN_SETTINGS_DEVICE_NAME_CAPACITY];
    char account_username[CN_CREDENTIAL_USERNAME_MAX + 1];
    unsigned char account_password[CN_ACCOUNT_PASSWORD_MAX + 1];
    size_t account_password_length;
    cn_account_text_input account_fields[4];
    int account_focus;
    int account_editing;
    int account_keyboard_active;
    int account_correction_confirm;
    int account_message;
    int account_action_focus;
    size_t account_value_start[3];
    int account_value_stop_count[3];
    int account_value_stop_x[3][ACCOUNT_VALUE_STOPS];
    cn_ui_account_local account_local_state;
    cn_ui_account_result account_status;
    cn_ui_account_action account_action;
};

/* ---- helpers ------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int btn_hit(int x, int y, int y0, int y1)
{
    return x >= BTN_X0 && x <= BTN_X1 && y >= y0 && y <= y1;
}

static void account_wipe(cn_ui *ui);

static void mark_at(cn_ui *ui, int x, int y, int *redraw)
{
    if (x == ui->mark_x && y == ui->mark_y)
        return;
    ui->mark_x = x;
    ui->mark_y = y;
    if (redraw)
        *redraw = 1;
}

static void account_submit(cn_ui *ui)
{
    ui->account_keyboard_active = 0;
    if (ui->account_correction_confirm == 1) {
        ui->account_correction_confirm = 2;
        ui->account_message = 19;
        return;
    }
    ui->account_action = ui->account_correction_confirm == 2
        ? CN_UI_ACCOUNT_REPLACE_DISABLED
        : CN_UI_ACCOUNT_SUBMIT_NEW_OR_RESUME;
    ui->account_message = 14;
}

enum account_option_kind {
    ACCOUNT_OPTION_BACK = 0,
    ACCOUNT_OPTION_RECHECK,
    ACCOUNT_OPTION_ACTIVATE,
    ACCOUNT_OPTION_CORRECT
};

typedef struct account_option {
    enum account_option_kind kind;
    const char *label;
} account_option;

static int account_options(const cn_ui *ui, account_option options[2])
{
    if (!ui || !options) return 0;
    if (ui->account_local_state == CN_ACCOUNT_SETUP_LOCAL_ENABLED) {
        options[0] = (account_option){ ACCOUNT_OPTION_BACK, "Back" };
        return 1;
    }
    if (ui->account_local_state == CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED) {
        switch (ui->account_status) {
        case CN_ACCOUNT_SETUP_PRECONDITION:
            options[0] = (account_option){ ACCOUNT_OPTION_ACTIVATE,
                                            "Retry activation" };
            options[1] = (account_option){ ACCOUNT_OPTION_CORRECT,
                                            "Correct saved account" };
            break;
        case CN_ACCOUNT_SETUP_AUTH_REJECTED:
            options[0] = (account_option){ ACCOUNT_OPTION_CORRECT,
                                            "Correct saved account" };
            options[1] = (account_option){ ACCOUNT_OPTION_BACK, "Back" };
            break;
        case CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED:
            options[0] = (account_option){ ACCOUNT_OPTION_ACTIVATE,
                                            "Retry activation" };
            options[1] = (account_option){ ACCOUNT_OPTION_BACK, "Back" };
            break;
        case CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN:
        case CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN:
        case CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN:
            options[0] = (account_option){ ACCOUNT_OPTION_RECHECK,
                                            "Recheck" };
            options[1] = (account_option){ ACCOUNT_OPTION_BACK, "Back" };
            break;
        default:
            options[0] = (account_option){ ACCOUNT_OPTION_RECHECK,
                                            "Recheck" };
            options[1] = (account_option){ ACCOUNT_OPTION_BACK, "Back" };
            break;
        }
        return 2;
    }
    options[0] = (account_option){ ACCOUNT_OPTION_RECHECK, "Recheck" };
    options[1] = (account_option){ ACCOUNT_OPTION_BACK, "Back" };
    return 2;
}

static int account_action_geometry(int count, int option, int *x0, int *y0,
                                   int *x1, int *y1)
{
    if (count < 1 || count > 2 || option < 0 || option >= count)
        return 0;
    *x0 = option ? 316 : 32;
    *x1 = option ? 568 : 284;
    *y0 = count == 1 ? ACCOUNT_SINGLE_ACTION_Y0 : ACCOUNT_ACTION_Y0;
    *y1 = count == 1 ? ACCOUNT_SINGLE_ACTION_Y1 : ACCOUNT_ACTION_Y1;
    return 1;
}

static void account_leave(cn_ui *ui)
{
    account_wipe(ui);
    ui->account_editing = 0;
    ui->account_keyboard_active = 0;
    ui->account_correction_confirm = 0;
    ui->state = CN_UI_HOME;
}

static void account_execute_option(cn_ui *ui, enum account_option_kind kind)
{
    if (kind == ACCOUNT_OPTION_BACK) {
        account_leave(ui);
    } else if (kind == ACCOUNT_OPTION_RECHECK) {
        ui->account_action = CN_UI_ACCOUNT_RECHECK;
        ui->account_message = 14;
    } else if (kind == ACCOUNT_OPTION_ACTIVATE) {
        ui->account_action = CN_UI_ACCOUNT_ACTIVATE_EXISTING;
        ui->account_message = 14;
    } else if (kind == ACCOUNT_OPTION_CORRECT) {
        memset(ui->account_url, 0, sizeof ui->account_url);
        memset(ui->account_device, 0, sizeof ui->account_device);
        memset(ui->account_username, 0, sizeof ui->account_username);
        cn_account_text_init(&ui->account_fields[0], ui->account_url,
                             sizeof ui->account_url);
        cn_account_text_init(&ui->account_fields[1], ui->account_device,
                             sizeof ui->account_device);
        cn_account_text_init(&ui->account_fields[2], ui->account_username,
                             sizeof ui->account_username);
        account_wipe(ui);
        cn_account_text_init(&ui->account_fields[3],
                             (char *)ui->account_password,
                             sizeof ui->account_password);
        cn_account_text_set_cursor_enabled(&ui->account_fields[3], 0);
        ui->account_editing = 1;
        ui->account_keyboard_active = 0;
        ui->account_correction_confirm = 1;
        ui->account_focus = 0;
    }
}

static int account_keyboard_geometry(const cn_account_text_input *field,
                                     int row, int column, int *x, int *y,
                                     int *width)
{
    static const int normal_last_x[7] = { 20, 98, 156, 236, 370, 450, 508 };
    static const int normal_last_w[7] = { 71, 51, 73, 127, 73, 51, 71 };

    if (!field || row < 0 || row >= CN_ACCOUNT_TEXT_KEY_ROWS ||
        column < 0 || column >= CN_ACCOUNT_TEXT_KEY_COLUMNS)
        return 0;
    if (row <= 1 || (field->symbols && row <= 3)) {
        *x = 20 + column * 56;
        *width = 51;
    } else if (row == 2) {
        *x = 48 + column * 56;
        *width = 51;
    } else if (row == 3 && !field->symbols) {
        if (column == 0) {
            *x = 20; *width = 71;
        } else if (column >= 1 && column <= 7) {
            *x = 98 + (column - 1) * 56; *width = 51;
        } else if (column == 8) {
            *x = 492; *width = 83;
        } else return 0;
    } else {
        if (column > 6) return 0;
        *x = normal_last_x[column];
        *width = normal_last_w[column];
    }
    *y = ACCOUNT_KEYBOARD_TOP + row * ACCOUNT_KEYBOARD_ROW_H;
    return 1;
}

static int account_keyboard_hit(const cn_account_text_input *field, int x, int y,
                                int *row_out, int *column_out)
{
    int row, column;
    for (row = 0; row < CN_ACCOUNT_TEXT_KEY_ROWS; ++row) {
        for (column = 0; column < CN_ACCOUNT_TEXT_KEY_COLUMNS; ++column) {
            int left, top, width;
            cn_account_text_key key;
            if (!account_keyboard_geometry(field, row, column,
                                           &left, &top, &width) ||
                !cn_account_text_key_at(field, row, column, &key))
                continue;
            if (x >= left && x <= left + width &&
                y >= top && y <= top + ACCOUNT_KEY_H) {
                *row_out = row;
                *column_out = column;
                return 1;
            }
        }
    }
    return 0;
}

static void account_activate_key(cn_ui *ui, cn_account_text_input *field)
{
    cn_account_text_result result = cn_account_text_activate(field);
    if (field == &ui->account_fields[3])
        ui->account_password_length = field->length;
    if (result == CN_ACCOUNT_TEXT_FULL) ui->account_message = 17;
    else if (result == CN_ACCOUNT_TEXT_INVALID) ui->account_message = 18;
    else if (result == CN_ACCOUNT_TEXT_DONE) ui->account_keyboard_active = 0;
}

static void account_place_cursor(cn_ui *ui, int field_index, int x)
{
    int i, best = 0;
    int best_distance;
    if (!ui || field_index < 0 || field_index >= 3 ||
        ui->account_value_stop_count[field_index] <= 0)
        return;
    best_distance = abs(x - ui->account_value_stop_x[field_index][0]);
    for (i = 1; i < ui->account_value_stop_count[field_index]; ++i) {
        int distance = abs(x - ui->account_value_stop_x[field_index][i]);
        if (distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    (void)cn_account_text_set_cursor(&ui->account_fields[field_index],
                                     ui->account_value_start[field_index] +
                                     (size_t)best);
}

static int account_field_hit(int x, int y)
{
    if (x < ACCOUNT_FIELD_X0 || x > ACCOUNT_FIELD_X1 ||
        y < ACCOUNT_FIELD_TOP || y >= ACCOUNT_FIELD_TOP + 4 * ACCOUNT_FIELD_H)
        return -1;
    return (y - ACCOUNT_FIELD_TOP) / ACCOUNT_FIELD_H;
}

static int account_submit_hit(int x, int y)
{
    return x >= ACCOUNT_SUBMIT_X0 && x <= ACCOUNT_SUBMIT_X1 &&
           y >= ACCOUNT_SUBMIT_Y0 && y <= ACCOUNT_SUBMIT_Y1;
}

static void lib_down(cn_ui *ui)
{
    int rows;
    if (!ui->lib || ui->lib->count == 0)
        return;
    if (ui->sel >= ui->lib->count - 1)
        return;                             /* already at the end */
    ui->sel++;
    rows = cn_ui_lib_rows();
    if (ui->sel - ui->top >= rows) {        /* crossed viewport bottom */
        ui->top = ui->sel;
        if (ui->top > ui->lib->count - rows)
            ui->top = ui->lib->count - rows;
        if (ui->top < 0)
            ui->top = 0;
    }
}

static void lib_up(cn_ui *ui)
{
    if (!ui->lib || ui->lib->count == 0)
        return;
    if (ui->sel <= 0)
        return;
    ui->sel--;
    if (ui->sel < ui->top)
        ui->top = ui->sel;
}

/* ---- UTF-8 encode helper (deterministic ellipsis strings) --------- */

static int utf8_encode(uint32_t cp, unsigned char *out)
{
    if (cp < 0x80) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (unsigned char)(0xF0 | (cp >> 18));
    out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (unsigned char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Render text on one baseline clipped (deterministically, by width) to
 * the [left_margin, width-right_margin] band; appends "…" when it does
 * not fit. Never draws outside the band and never writes past the canvas
 * (the rasterizer also clips per glyph). */
static void render_fit(cn_text *t, cn_canvas *c, const char *text,
                       int baseline, int l, int r,
                       uint16_t fg, uint16_t bg)
{
    uint32_t cps[MAX_LIB_CP];
    unsigned char buf[MAX_LIB_CP * 4 + 4];
    int n, bad, i, k, acc, pos;
    int avail = (cn_canvas_width(c) - r) - l;
    int ell = cn_text_measure(t, 0x2026);
    uint32_t ell_cp = 0x2026;
    int total = 0;

    if (avail <= 0 || !text)
        return;
    if (ell <= 0) {                 /* font without U+2026: use "..." */
        ell = cn_text_measure(t, '.') * 3;
        ell_cp = '.';
    }
    n = cn_utf8_decode(text, cps, MAX_LIB_CP, &bad);
    if (bad > 0)
        fprintf(stderr, "ui: %d malformed byte(s) skipped\n", bad);

    for (i = 0; i < n; i++)
        total += cn_text_measure(t, cps[i]);
    if (total <= avail) {
        (void)cn_text_render(t, c, text, &baseline, l, r, fg, bg);
        return;
    }
    if (ell > avail)                /* nothing fits: draw nothing */
        return;

    k = 0;
    acc = 0;
    while (k < n) {
        int w = cn_text_measure(t, cps[k]);
        if (acc + w + ell > avail)
            break;
        acc += w;
        k++;
    }
    pos = 0;
    for (i = 0; i < k; i++) {
        int e = utf8_encode(cps[i], buf + pos);
        if (e <= 0)
            break;
        pos += e;
    }
    if (ell_cp == '.') {
        buf[pos++] = '.';
        buf[pos++] = '.';
        buf[pos++] = '.';
    } else {
        pos += utf8_encode(ell_cp, buf + pos);
    }
    buf[pos] = '\0';
    (void)cn_text_render(t, c, (const char *)buf, &baseline, l, r, fg, bg);
}

static int utf8_text_width(const cn_text *t, const char *text)
{
    uint32_t cps[MAX_LIB_CP];
    int bad, i, n, width = 0;
    if (!text) return 0;
    n = cn_utf8_decode(text, cps, MAX_LIB_CP, &bad);
    if (n > MAX_LIB_CP) n = MAX_LIB_CP;
    for (i = 0; i < n; ++i)
        width += cn_text_measure(t, cps[i]);
    return width;
}

static void render_centered(cn_text *t, cn_canvas *c, const char *text,
                            int baseline, int x0, int x1,
                            uint16_t fg, uint16_t bg)
{
    int width = utf8_text_width(t, text);
    int x = x0 + ((x1 - x0 + 1) - width) / 2;
    if (x < x0) x = x0;
    (void)cn_text_render(t, c, text, &baseline, x,
                         cn_canvas_width(c) - x1 - 1, fg, bg);
}

/* ---- public API --------------------------------------------------- */

cn_ui *cn_ui_init(void)
{
    cn_ui *ui = (cn_ui *)calloc(1, sizeof *ui);
    if (!ui)
        return NULL;
    ui->state = CN_UI_HOME;
    ui->page = 1;
    ui->mark_x = -1;
    ui->mark_y = -1;
    ui->power_press_ms = -1;
    ui->account_local_state = CN_ACCOUNT_SETUP_LOCAL_UNKNOWN;
    ui->account_message = 0;
    cn_account_text_init(&ui->account_fields[0], ui->account_url,
                         sizeof ui->account_url);
    cn_account_text_init(&ui->account_fields[1], ui->account_device,
                         sizeof ui->account_device);
    cn_account_text_init(&ui->account_fields[2], ui->account_username,
                         sizeof ui->account_username);
    cn_account_text_init(&ui->account_fields[3], (char *)ui->account_password,
                         sizeof ui->account_password);
    cn_account_text_set_cursor_enabled(&ui->account_fields[3], 0);
    return ui;
}

static void account_wipe(cn_ui *ui)
{
    volatile unsigned char *p;
    size_t n;
    if (!ui) return;
    p = ui->account_password;
    for (n = 0; n < sizeof ui->account_password; ++n) p[n] = 0;
    ui->account_password_length = 0;
    ui->account_fields[3].length = 0;
    ui->account_fields[3].cursor = 0;
}

void cn_ui_free(cn_ui *ui)
{
    if (!ui)
        return;
    account_wipe(ui);
    cn_reader_free(ui->reader);
    free(ui);
}

cn_ui_account_action cn_ui_account_take_action(cn_ui *ui)
{
    cn_ui_account_action action;
    if (!ui) return CN_UI_ACCOUNT_NONE;
    action = ui->account_action;
    ui->account_action = CN_UI_ACCOUNT_NONE;
    return action;
}

int cn_ui_account_action_pending(const cn_ui *ui)
{ return ui && ui->account_action != CN_UI_ACCOUNT_NONE; }

int cn_ui_account_get_input(cn_ui *ui, cn_ui_account_input *input)
{
    if (!ui || !input || ui->state != CN_UI_ACCOUNT_SETUP) return -1;
    input->base_url = ui->account_url;
    input->device_name = ui->account_device;
    input->username = ui->account_username;
    input->password = ui->account_password;
    input->password_length = ui->account_password_length;
    return 0;
}

void cn_ui_account_clear_password(cn_ui *ui) { account_wipe(ui); }

void cn_ui_account_prefill(cn_ui *ui, const char *url, const char *device)
{
    size_t n;
    if (!ui || ui->state != CN_UI_ACCOUNT_SETUP) return;
    if (url && (n = strlen(url)) < sizeof ui->account_url) {
        memcpy(ui->account_url, url, n + 1);
        ui->account_fields[0].length = ui->account_fields[0].cursor = n;
    }
    if (device && (n = strlen(device)) < sizeof ui->account_device) {
        memcpy(ui->account_device, device, n + 1);
        ui->account_fields[1].length = ui->account_fields[1].cursor = n;
    }
}

int cn_ui_account_local_state(const cn_ui *ui)
{ return ui ? ui->account_local_state : CN_ACCOUNT_SETUP_LOCAL_UNKNOWN; }

void cn_ui_account_set_result(cn_ui *ui, cn_ui_account_result status,
                              cn_ui_account_local local_state)
{
    int correcting;
    if (!ui) return;
    correcting = ui->account_correction_confirm != 0;
    ui->account_keyboard_active = 0;
    ui->account_status = status;
    ui->account_local_state = local_state;
    if (status == CN_ACCOUNT_SETUP_ACTIVATED ||
        status == CN_ACCOUNT_SETUP_ALREADY_ENABLED ||
        status == CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE)
        ui->account_local_state = CN_ACCOUNT_SETUP_LOCAL_ENABLED;
    ui->account_action_focus = 0;
    ui->account_correction_confirm = 0;
    switch (status) {
    case CN_ACCOUNT_SETUP_ACTIVATED: ui->account_message = 15; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_ALREADY_ENABLED: ui->account_message = 16; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_AUTH_REJECTED: ui->account_message = 8; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED: ui->account_message = 9; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN:
    case CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN:
    case CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN:
        ui->account_message = 10; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE: ui->account_message = 12; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE: ui->account_message = 6; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_INCOMPLETE_STATE: ui->account_message = local_state == CN_ACCOUNT_SETUP_LOCAL_ORPHAN_CREDENTIALS ? 3 : 2; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_INVALID_INPUT: ui->account_message = 13; account_wipe(ui); break;
    case CN_ACCOUNT_SETUP_PRECONDITION:
        if (local_state == CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED) ui->account_message = 4;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT) ui->account_message = 1;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED) ui->account_message = 2;
        else ui->account_message = 11;
        account_wipe(ui);
        break;
    case CN_ACCOUNT_SETUP_BOOTSTRAP_FAILED:
    case CN_ACCOUNT_SETUP_IDENTITY_FAILED:
    case CN_ACCOUNT_SETUP_ACTIVATION_SAVE_FAILED:
    case CN_ACCOUNT_SETUP_ACTIVATION_FAILED:
        ui->account_message = 11;
        account_wipe(ui);
        break;
    default:
        if (local_state == CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT) ui->account_message = 1;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED) ui->account_message = 2;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED) ui->account_message = 4;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_ENABLED) ui->account_message = 5;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_ORPHAN_CREDENTIALS) ui->account_message = 3;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_CORRUPT_OR_UNSUPPORTED) ui->account_message = 6;
        else if (local_state == CN_ACCOUNT_SETUP_LOCAL_UNREADABLE) ui->account_message = 7;
        else ui->account_message = 11;
        break;
    }
    ui->account_editing = ui->account_local_state == CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT ||
                          ui->account_local_state == CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED;
    if (status == CN_ACCOUNT_SETUP_INVALID_INPUT && correcting &&
        ui->account_local_state == CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED) {
        ui->account_editing = 1;
        ui->account_correction_confirm = 1;
        ui->account_action_focus = 0;
    }
    if (status == CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN ||
        status == CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN ||
        status == CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN)
        ui->account_editing = 0;
}

int cn_ui_set_library(cn_ui *ui, const cn_library *lib)
{
    if (ui) {
        ui->sync_modal = 0;
        ui->sync_action = CN_UI_SYNC_NONE;
        ui->lib = lib;
        ui->sel = 0;
        ui->top = 0;
    }
    return (lib && ui) ? lib->count : 0;
}

int cn_ui_set_reader(cn_ui *ui, const cn_reader_config *cfg)
{
    if (!ui)
        return -1;
    ui->sync_modal = 0;
    ui->sync_action = CN_UI_SYNC_NONE;
    if (ui->reader) {
        cn_reader_free(ui->reader);
        ui->reader = NULL;
    }
    if (cfg) {
        ui->reader = cn_reader_new(cfg);
        if (!ui->reader)
            return -1;
    }
    return 0;
}

int cn_ui_lib_rows(void)
{
    int rows = (CN_UI_LIB_FOOTER_Y - CN_UI_LIB_ROW_TOP) / CN_UI_LIB_ROW_H;
    return rows > 0 ? rows : 1;
}

int cn_ui_show_sync_conflict(cn_ui *ui)
{
    if (!ui || ui->state != CN_UI_READER || !ui->reader ||
        !cn_reader_is_open(ui->reader)) return -1;
    ui->sync_action = CN_UI_SYNC_NONE;
    ui->sync_modal = 1;
    ui->sync_selection = 2;
    return 0;
}

int cn_ui_show_sync_feedback(cn_ui *ui, cn_ui_sync_feedback feedback)
{
    if (!ui || ui->state != CN_UI_READER || !ui->reader ||
        !cn_reader_is_open(ui->reader) || feedback < CN_UI_SYNC_FEEDBACK_WORKING ||
        feedback > CN_UI_SYNC_FEEDBACK_FAILED) return -1;
    ui->sync_modal = 2;
    ui->sync_action = CN_UI_SYNC_NONE;
    ui->sync_feedback = feedback;
    return 0;
}

int cn_ui_sync_modal_active(const cn_ui *ui)
{
    return ui && ui->sync_modal != 0;
}

int cn_ui_sync_selection(const cn_ui *ui)
{
    return ui && ui->sync_modal == 1 ? ui->sync_selection : -1;
}

cn_ui_sync_action cn_ui_take_sync_action(cn_ui *ui)
{
    cn_ui_sync_action action;
    if (!ui) return CN_UI_SYNC_NONE;
    action = ui->sync_action;
    ui->sync_action = CN_UI_SYNC_NONE;
    return action;
}

int cn_ui_handle(cn_ui *ui, const cn_input_ev *ev)
{
    int redraw = 0;

    if (!ui || !ev) return 0;
    if (ui->state == CN_UI_ACCOUNT_SETUP &&
        ev->type != CN_INPUT_POWER_DOWN && ev->type != CN_INPUT_POWER_UP) {
        if (ev->type == CN_INPUT_BACK) {
            if (ui->account_keyboard_active) {
                ui->account_keyboard_active = 0;
                return 1;
            }
            account_leave(ui);
            return 1;
        }
        if (ev->type == CN_INPUT_HOME) {
            account_leave(ui);
            return 1;
        }
        if (ui->account_action != CN_UI_ACCOUNT_NONE)
            return 0;
        if (ui->account_keyboard_active) {
            if (ev->type == CN_INPUT_PAGE_NEXT) {
                (void)cn_account_text_move(&ui->account_fields[ui->account_focus], 1);
                return 1;
            }
            if (ev->type == CN_INPUT_PAGE_PREV) {
                (void)cn_account_text_move(&ui->account_fields[ui->account_focus], -1);
                return 1;
            }
            if (ev->type == CN_INPUT_MENU) {
                account_activate_key(ui,
                    &ui->account_fields[ui->account_focus]);
                return 1;
            }
            if (ev->type == CN_INPUT_TOUCH_UP) {
                int row, column;
                int field_index;
                int changed_mode = ui->account_keyboard_active;
                ui->account_keyboard_active = 0;
                if (ui->account_focus < 4 && account_keyboard_hit(
                        &ui->account_fields[ui->account_focus], ev->x, ev->y,
                        &row, &column)) {
                    cn_account_text_input *field =
                        &ui->account_fields[ui->account_focus];
                    (void)cn_account_text_select(field, row, column);
                    account_activate_key(ui, field);
                    return 1;
                }
                field_index = account_field_hit(ev->x, ev->y);
                if (field_index >= 0) {
                    ui->account_focus = field_index;
                    if (field_index < 3 &&
                        ev->y >= ACCOUNT_FIELD_TOP +
                                 field_index * ACCOUNT_FIELD_H + 24 &&
                        ev->y <= ACCOUNT_FIELD_TOP +
                                 field_index * ACCOUNT_FIELD_H + 62)
                        account_place_cursor(ui, field_index, ev->x);
                    return 1;
                }
                if (account_submit_hit(ev->x, ev->y)) {
                    account_submit(ui);
                    return 1;
                }
                return changed_mode;
            }
            return 0;
        }
        if (ui->account_editing) {
            if (ev->type == CN_INPUT_PAGE_NEXT || ev->type == CN_INPUT_PAGE_PREV) {
                int step = ev->type == CN_INPUT_PAGE_NEXT ? 1 : -1;
                ui->account_focus = (ui->account_focus + step + 5) % 5;
                return 1;
            }
            if (ev->type == CN_INPUT_MENU) {
                if (ui->account_focus == 4) {
                    account_submit(ui);
                    return 1;
                }
                ui->account_keyboard_active = 1;
                return 1;
            }
            if (ev->type == CN_INPUT_TOUCH_UP) {
                int field_index = account_field_hit(ev->x, ev->y);
                if (field_index >= 0) {
                    ui->account_focus = field_index;
                    if (field_index < 3 &&
                        ev->y >= ACCOUNT_FIELD_TOP +
                                 field_index * ACCOUNT_FIELD_H + 24 &&
                        ev->y <= ACCOUNT_FIELD_TOP +
                                 field_index * ACCOUNT_FIELD_H + 62)
                        account_place_cursor(ui, field_index, ev->x);
                    return 1;
                }
                if (ui->account_focus < 4) {
                    int row, column;
                    if (account_keyboard_hit(&ui->account_fields[ui->account_focus],
                                             ev->x, ev->y, &row, &column)) {
                        cn_account_text_input *field =
                            &ui->account_fields[ui->account_focus];
                        (void)cn_account_text_select(field, row, column);
                        account_activate_key(ui, field);
                        return 1;
                    }
                }
                if (account_submit_hit(ev->x, ev->y)) {
                    account_submit(ui);
                    return 1;
                }
            }
            return 0;
        }
        {
            account_option options[2];
            int count = account_options(ui, options);
            if (!count) return 0;
            if (ui->account_action_focus >= count)
                ui->account_action_focus = count - 1;
            if (ev->type == CN_INPUT_PAGE_NEXT || ev->type == CN_INPUT_PAGE_PREV) {
                int step = ev->type == CN_INPUT_PAGE_NEXT ? 1 : -1;
                ui->account_action_focus = (ui->account_action_focus + step + count) % count;
                return 1;
            }
            if (ev->type == CN_INPUT_MENU) {
                account_execute_option(ui, options[ui->account_action_focus].kind);
                return 1;
            }
            if (ev->type == CN_INPUT_TOUCH_UP) {
                int option;
                for (option = 0; option < count; ++option) {
                    int x0, y0, x1, y1;
                    if (account_action_geometry(count, option, &x0, &y0,
                                                &x1, &y1) &&
                        ev->x >= x0 && ev->x <= x1 &&
                        ev->y >= y0 && ev->y <= y1)
                        break;
                }
                if (option < count) {
                    ui->account_action_focus = option;
                    account_execute_option(ui, options[option].kind);
                    return 1;
                }
            }
        }
        return 0;
    }
    /* Preserve long-power processing; no other modal input reaches Reader. */
    if (ui->state == CN_UI_READER && ui->sync_modal &&
        ev->type != CN_INPUT_POWER_DOWN && ev->type != CN_INPUT_POWER_UP) {
        if (ev->type == CN_INPUT_BACK || ev->type == CN_INPUT_HOME) {
            if (ui->sync_modal == 1) ui->sync_action = CN_UI_SYNC_CANCEL;
            ui->sync_modal = 0;
            return 1;
        }
        if (ui->sync_modal == 2) return 0;
        if (ev->type == CN_INPUT_PAGE_NEXT && ui->sync_selection < 2) {
            ++ui->sync_selection;
            return 1;
        }
        if (ev->type == CN_INPUT_PAGE_PREV && ui->sync_selection > 0) {
            --ui->sync_selection;
            return 1;
        }
        if (ev->type == CN_INPUT_TOUCH_UP && ev->x >= MARGIN_L &&
            ev->x < CN_READER_W - MARGIN_R && ev->y >= SYNC_ROW_TOP &&
            ev->y < SYNC_ROW_TOP + 3 * SYNC_ROW_H) {
            int selected = (ev->y - SYNC_ROW_TOP) / SYNC_ROW_H;
            if (selected != ui->sync_selection) {
                ui->sync_selection = selected;
                return 1;
            }
            return 0;
        }
        if (ev->type == CN_INPUT_MENU) {
            ui->sync_action = ui->sync_selection == 0 ? CN_UI_SYNC_USE_LOCAL :
                              ui->sync_selection == 1 ? CN_UI_SYNC_USE_REMOTE :
                                                        CN_UI_SYNC_CANCEL;
            if (ui->sync_action == CN_UI_SYNC_CANCEL) {
                ui->sync_modal = 0;
                return 1;
            }
            /* Disarm before the app invokes any controller. Repeated MENU
             * cannot authorize a second request. */
            ui->sync_modal = 2;
            ui->sync_feedback = CN_UI_SYNC_FEEDBACK_WORKING;
            return 1;
        }
        return 0;
    }

    switch (ev->type) {
    case CN_INPUT_PAGE_NEXT:
        if (ui->state == CN_UI_READER) {
            if (ui->reader && cn_reader_is_open(ui->reader)) {
                int p = cn_reader_page(ui->reader);
                if (cn_reader_next(ui->reader) != p)
                    redraw = 1;
            }
        } else if (ui->state == CN_UI_LIBRARY) {
            lib_down(ui);
            redraw = 1;
        } else if (ui->state == CN_UI_HOME) {
            ui->state = CN_UI_READER_TEST;
            ui->page++;
            redraw = 1;
        } else if (ui->state == CN_UI_READER_TEST) {
            ui->page++;
            redraw = 1;
        }
        break;

    case CN_INPUT_PAGE_PREV:
        if (ui->state == CN_UI_READER) {
            if (ui->reader && cn_reader_is_open(ui->reader)) {
                int p = cn_reader_page(ui->reader);
                if (cn_reader_prev(ui->reader) != p)
                    redraw = 1;
            }
        } else if (ui->state == CN_UI_READER_TEST) {
            if (ui->page > 1)
                ui->page--;
            redraw = 1;
        } else if (ui->state == CN_UI_LIBRARY) {
            lib_up(ui);
            redraw = 1;
        }
        break;

    case CN_INPUT_MENU:
        if (ui->state == CN_UI_READER)
            ui->sync_action = CN_UI_SYNC_MANUAL;
        else if (ui->state == CN_UI_HOME) {
            ui->state = CN_UI_ACCOUNT_SETUP;
            ui->account_action = CN_UI_ACCOUNT_INSPECT;
            ui->account_message = 14;
            redraw = 1;
        }
        break;

    case CN_INPUT_BACK:
        if (ui->state == CN_UI_READER) {
            if (ui->reader)
                cn_reader_close(ui->reader);
            ui->state = CN_UI_LIBRARY;
            redraw = 1;
        } else if (ui->state == CN_UI_SELECTED_BOOK) {
            ui->state = CN_UI_LIBRARY;
            redraw = 1;
        } else if (ui->state != CN_UI_HOME) {
            ui->state = CN_UI_HOME;
            redraw = 1;
        }
        break;

    case CN_INPUT_HOME:
        if (ui->state == CN_UI_READER) {
            if (ui->reader)
                cn_reader_close(ui->reader);
            ui->state = CN_UI_HOME;
            redraw = 1;
        } else if (ui->state != CN_UI_HOME) {
            ui->state = CN_UI_HOME;
            redraw = 1;
        }
        break;

    case CN_INPUT_POWER_DOWN:
        ui->power_press_ms = now_ms();
        break;

    case CN_INPUT_POWER_UP:
        if (ui->power_press_ms >= 0) {
            long long dur = now_ms() - ui->power_press_ms;
            if (dur < 0)
                dur = 0;
            if (dur >= CN_UI_POWER_LONG_MS)
                ui->exit_requested = 1;
            if (ui->exit_requested && ui->state == CN_UI_ACCOUNT_SETUP)
                account_wipe(ui);
            ui->power_press_ms = -1;
        }
        break;

    case CN_INPUT_TOUCH_DOWN:
        /* provisional; finalized by TOUCH_UP */
        break;

    case CN_INPUT_TOUCH_MOVE:
        break;

    case CN_INPUT_TOUCH_UP:
        if (ui->state != CN_UI_READER)
            mark_at(ui, ev->x, ev->y, &redraw);
        if (ui->state == CN_UI_HOME) {
            if (btn_hit(ev->x, ev->y, BTN_R_Y0, BTN_R_Y1)) {
                ui->state = CN_UI_READER_TEST;
                redraw = 1;
            } else if (ui->lib &&
                       btn_hit(ev->x, ev->y, BTN_L_Y0, BTN_L_Y1)) {
                ui->state = CN_UI_LIBRARY;
                ui->sel = 0;
                ui->top = 0;
                redraw = 1;
            } else if (btn_hit(ev->x, ev->y, 420, 464)) {
                ui->state = CN_UI_ACCOUNT_SETUP;
                ui->account_action = CN_UI_ACCOUNT_INSPECT;
                ui->account_message = 14;
                redraw = 1;
            }
        } else if (ui->state == CN_UI_LIBRARY) {
            int rows = cn_ui_lib_rows();
            int r = (ev->y - CN_UI_LIB_ROW_TOP) / CN_UI_LIB_ROW_H;
            if (ev->y >= CN_UI_LIB_ROW_TOP && r >= 0 && r < rows) {
                int idx = ui->top + r;
                if (ui->lib && idx < ui->lib->count) {
                    if (idx == ui->sel) {
                        const cn_book *b = &ui->lib->books[idx];
                        if (ui->reader && b->format == CN_BOOK_EPUB) {
                            if (cn_reader_open(ui->reader, b->path) == 0)
                                ui->state = CN_UI_READER;
                            else     /* deterministic fallback, never blank */
                                ui->state = CN_UI_SELECTED_BOOK;
                        } else {
                            ui->state = CN_UI_SELECTED_BOOK;
                        }
                    } else {
                        ui->sel = idx;
                    }
                    redraw = 1;
                }
            }
        }
        break;

    case CN_INPUT_NONE:
    default:
        break;
    }
    return redraw;
}

/* ---- rendering (state -> canvas) -------------------------------- */

static void draw_marker(cn_canvas *c, const cn_ui *ui)
{
    if (ui->mark_x < 0 || ui->mark_y < 0)
        return;
    cn_canvas_fill_rect(c,
                        ui->mark_x - MARK_HALF, ui->mark_y - MARK_HALF,
                        ui->mark_x + MARK_HALF, ui->mark_y + MARK_HALF,
                        CN_COLOR_BLACK);
    cn_canvas_fill_rect(c,
                        ui->mark_x - 1, ui->mark_y - 1,
                        ui->mark_x + 1, ui->mark_y + 1,
                        CN_COLOR_WHITE);
}

static void render_home(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    int y = MARGIN_TOP;
    int a;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    a = cn_text_set_size(t, 48);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "CrossNook", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    y += 14;
    a = cn_text_set_size(t, 24);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "UI Core", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    /* button */
    cn_canvas_outline_rect(c, BTN_X0, BTN_R_Y0, BTN_X1, BTN_R_Y1,
                           CN_COLOR_BLACK);
    y = BTN_R_Y0 + (BTN_R_Y1 - BTN_R_Y0) / 2 + a / 2 - 6;
    (void)cn_text_render(t, c, "[ Open reader test ]", &y,
                         MARGIN_L + 16, MARGIN_R, CN_COLOR_BLACK,
                         CN_COLOR_WHITE);

    if (ui->lib) {      /* only when a library is attached */
        cn_canvas_outline_rect(c, BTN_X0, BTN_L_Y0, BTN_X1, BTN_L_Y1,
                               CN_COLOR_BLACK);
        y = BTN_L_Y0 + (BTN_L_Y1 - BTN_L_Y0) / 2 + a / 2 - 6;
        (void)cn_text_render(t, c, "[ Open library ]", &y,
                             MARGIN_L + 16, MARGIN_R, CN_COLOR_BLACK,
                             CN_COLOR_WHITE);
    }

    cn_canvas_outline_rect(c, BTN_X0, 420, BTN_X1, 464, CN_COLOR_BLACK);
    y = 438 + a / 2 - 6;
    (void)cn_text_render(t, c, "[ Sync account ]", &y, MARGIN_L + 16,
                         MARGIN_R, CN_COLOR_BLACK, CN_COLOR_WHITE);

    draw_marker(c, ui);
}

static void account_value_window(cn_ui *ui, int field_index, cn_text *t,
                                 int x0, int x1, size_t *start_out,
                                 size_t *end_out, int *caret_x_out)
{
    cn_account_text_input *field = &ui->account_fields[field_index];
    size_t start = field->cursor;
    size_t end = field->cursor;
    int available = x1 - x0 - 3;
    int prefix_width = 0;
    int suffix_width = 0;
    int prefix_target = available * 2 / 3;
    int x = x0;
    int count = 0;
    size_t i;

    while (start > 0 && field->cursor - start < ACCOUNT_VALUE_STOPS - 1) {
        int width = cn_text_measure(t, (unsigned char)field->bytes[start - 1]);
        if (width < 1) width = 1;
        if (prefix_width + width > prefix_target) break;
        --start;
        prefix_width += width;
    }
    while (end < field->length && end - start < ACCOUNT_VALUE_STOPS - 1) {
        int width = cn_text_measure(t, (unsigned char)field->bytes[end]);
        if (width < 1) width = 1;
        if (prefix_width + suffix_width + width > available) break;
        suffix_width += width;
        ++end;
    }
    while (start > 0 && end - start < ACCOUNT_VALUE_STOPS - 1) {
        int width = cn_text_measure(t, (unsigned char)field->bytes[start - 1]);
        if (width < 1) width = 1;
        if (prefix_width + suffix_width + width > available) break;
        --start;
        prefix_width += width;
    }

    ui->account_value_start[field_index] = start;
    ui->account_value_stop_x[field_index][count++] = x;
    for (i = start; i < end && count < ACCOUNT_VALUE_STOPS; ++i) {
        int width = cn_text_measure(t, (unsigned char)field->bytes[i]);
        if (width < 1) width = 1;
        x += width;
        ui->account_value_stop_x[field_index][count++] = x;
    }
    ui->account_value_stop_count[field_index] = count;
    *start_out = start;
    *end_out = end;
    *caret_x_out = ui->account_value_stop_x[field_index]
        [field->cursor - start];
}

static void render_account(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    static const char *const labels[4] = { "Base URL", "Device name", "Username", "Password" };
    char password_mask[CN_ACCOUNT_PASSWORD_MAX + 1];
    size_t password_mask_length = ui->account_password_length;
    int i, y = MARGIN_TOP;
    int ascent;
    if (password_mask_length > CN_ACCOUNT_PASSWORD_MAX)
        password_mask_length = CN_ACCOUNT_PASSWORD_MAX;
    memset(password_mask, '*', password_mask_length);
    password_mask[password_mask_length] = 0;
    cn_canvas_clear(c, CN_COLOR_WHITE);

    ascent = cn_text_set_size(t, 40);
    if (ascent > 0) y += ascent;
    (void)cn_text_render(t, c, "Sync account", &y, MARGIN_L, MARGIN_R,
                          CN_COLOR_BLACK, CN_COLOR_WHITE);

    (void)cn_text_set_size(t, 18);
    y = 108;
    render_fit(t, c, account_messages[ui->account_message], y,
               MARGIN_L, MARGIN_R, CN_COLOR_BLACK, CN_COLOR_WHITE);

    if (ui->account_editing) {
        for (i = 0; i < 4; ++i) {
            int top = ACCOUNT_FIELD_TOP + i * ACCOUNT_FIELD_H;
            int selected = i == ui->account_focus;
            uint16_t bg = CN_COLOR_WHITE;
            const char *value = i == 0 ? ui->account_url :
                                i == 1 ? ui->account_device :
                                i == 2 ? ui->account_username :
                                password_mask;

            if (selected) {
                cn_canvas_fill_rect(c, ACCOUNT_FIELD_X0, top,
                                    ACCOUNT_FIELD_X0 + 5, top + 62,
                                    CN_COLOR_BLACK);
            }
            cn_canvas_hline(c, ACCOUNT_FIELD_X0 + 8, ACCOUNT_FIELD_X1,
                            top + 63, CN_COLOR_BLACK);

            (void)cn_text_set_size(t, 18);
            y = top + 19;
            (void)cn_text_render(t, c, labels[i], &y,
                                 ACCOUNT_FIELD_X0 + 12,
                                 cn_canvas_width(c) - ACCOUNT_FIELD_X1 - 1,
                                 CN_COLOR_BLACK, CN_COLOR_WHITE);
            ascent = cn_text_set_size(t, 22);
            y = top + 24 + (39 - 22) / 2 + (ascent > 0 ? ascent : 0);
            if (i == 3) {
                if (value && *value) {
                    render_fit(t, c, value, y, ACCOUNT_FIELD_X0 + 12,
                               cn_canvas_width(c) - ACCOUNT_FIELD_X1 - 1,
                               CN_COLOR_BLACK, bg);
                }
            } else {
                size_t start, end;
                int caret_x;
                char window[ACCOUNT_VALUE_STOPS];
                size_t n;
                account_value_window(ui, i, t, ACCOUNT_FIELD_X0 + 12,
                                     ACCOUNT_FIELD_X1, &start, &end,
                                     &caret_x);
                n = end - start;
                if (n) {
                    memcpy(window, value + start, n);
                    window[n] = 0;
                    (void)cn_text_render(t, c, window, &y,
                                         ACCOUNT_FIELD_X0 + 12,
                                         cn_canvas_width(c) - ACCOUNT_FIELD_X1 - 1,
                                         CN_COLOR_BLACK, bg);
                }
                if (selected) {
                    cn_canvas_vline(c, caret_x, top + 29, top + 58,
                                    CN_COLOR_BLACK);
                    if (caret_x < ACCOUNT_FIELD_X1)
                        cn_canvas_vline(c, caret_x + 1, top + 29, top + 58,
                                        CN_COLOR_BLACK);
                }
            }
        }

        {
            uint16_t bg = ui->account_focus == 4 ? CN_COLOR_BLACK : CN_COLOR_WHITE;
            uint16_t fg = ui->account_focus == 4 ? CN_COLOR_WHITE : CN_COLOR_BLACK;
            cn_canvas_fill_rect(c, ACCOUNT_SUBMIT_X0, ACCOUNT_SUBMIT_Y0,
                                ACCOUNT_SUBMIT_X1, ACCOUNT_SUBMIT_Y1, bg);
            cn_canvas_outline_rect(c, ACCOUNT_SUBMIT_X0, ACCOUNT_SUBMIT_Y0,
                                   ACCOUNT_SUBMIT_X1, ACCOUNT_SUBMIT_Y1,
                                   CN_COLOR_BLACK);
            ascent = cn_text_set_size(t, 22);
            y = ACCOUNT_SUBMIT_Y0 +
                ((ACCOUNT_SUBMIT_Y1 - ACCOUNT_SUBMIT_Y0 + 1) - 22) / 2 +
                (ascent > 0 ? ascent : 0);
            render_centered(t, c, "Continue", y, ACCOUNT_SUBMIT_X0,
                            ACCOUNT_SUBMIT_X1, fg, bg);
        }

        for (i = 0; i < CN_ACCOUNT_TEXT_KEY_ROWS; ++i) {
            int col;
            int field_index = ui->account_focus < 4 ? ui->account_focus : 3;
            cn_account_text_input *field = &ui->account_fields[field_index];
            for (col = 0; col < CN_ACCOUNT_TEXT_KEY_COLUMNS; ++col) {
                int x, yy, width;
                char character[2];
                const char *label;
                uint16_t bg = CN_COLOR_WHITE;
                cn_account_text_key key;
                if (!cn_account_text_key_at(field, i, col, &key) ||
                    !account_keyboard_geometry(field, i, col,
                                               &x, &yy, &width))
                    continue;
                label = key.label;
                if (!label) {
                    character[0] = (char)key.character;
                    character[1] = 0;
                    label = character;
                }
                if (ui->account_keyboard_active &&
                    cn_account_text_selection(field) ==
                        i * CN_ACCOUNT_TEXT_SELECTION_STRIDE + col)
                    bg = CN_COLOR_BLACK;
                if (key.kind == CN_ACCOUNT_TEXT_KEY_SHIFT && field->shifted)
                    bg = CN_COLOR_BLACK;
                cn_canvas_fill_rect(c, x, yy, x + width,
                                    yy + ACCOUNT_KEY_H, bg);
                cn_canvas_outline_rect(c, x, yy, x + width,
                                       yy + ACCOUNT_KEY_H,
                                       CN_COLOR_BLACK);
                {
                    int px = key.kind == CN_ACCOUNT_TEXT_KEY_CURSOR_LEFT ||
                             key.kind == CN_ACCOUNT_TEXT_KEY_CURSOR_RIGHT ? 24 :
                             key.label && key.kind !=
                             CN_ACCOUNT_TEXT_KEY_CHARACTER ? 15 : 20;
                    uint16_t fg = bg == CN_COLOR_BLACK ? CN_COLOR_WHITE :
                                                               CN_COLOR_BLACK;
                    ascent = cn_text_set_size(t, px);
                    y = yy + ((ACCOUNT_KEY_H + 1) - px) / 2 +
                        (ascent > 0 ? ascent : 0);
                    render_centered(t, c, label, y, x, x + width, fg, bg);
                }
            }
        }
        (void)cn_text_set_size(t, 16);
        y = 782;
        (void)cn_text_render(t, c, ui->account_keyboard_active
                                   ? "NEXT/PREV key  MENU activate  BACK form"
                                   : "NEXT/PREV focus  MENU keyboard  BACK form/home",
                             &y, MARGIN_L, MARGIN_R, CN_COLOR_BLACK, CN_COLOR_WHITE);
    } else {
        account_option options[2];
        int count = account_options(ui, options);
        for (i = 0; i < count; ++i) {
            int x0, y0, x1, y1;
            uint16_t bg = ui->account_action_focus == i ?
                          CN_COLOR_BLACK : CN_COLOR_WHITE;
            uint16_t fg = bg == CN_COLOR_BLACK ? CN_COLOR_WHITE : CN_COLOR_BLACK;
            if (!account_action_geometry(count, i, &x0, &y0, &x1, &y1))
                continue;
            cn_canvas_fill_rect(c, x0, y0, x1, y1, bg);
            cn_canvas_outline_rect(c, x0, y0, x1, y1, CN_COLOR_BLACK);
            ascent = cn_text_set_size(t, 18);
            y = y0 + ((y1 - y0 + 1) - 18) / 2 +
                (ascent > 0 ? ascent : 0);
            render_centered(t, c, options[i].label, y, x0, x1, fg, bg);
        }
        (void)cn_text_set_size(t, 16);
        y = count == 1 ? ACCOUNT_SINGLE_ACTION_FOOTER_Y : 300;
        (void)cn_text_render(t, c, "NEXT/PREV choose  MENU confirm  BACK/HOME leave",
                             &y, MARGIN_L, MARGIN_R, CN_COLOR_BLACK, CN_COLOR_WHITE);
    }
}

static void render_reader(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    char page_buf[32];
    int y = MARGIN_TOP;
    int a;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    a = cn_text_set_size(t, 36);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "Reader test", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    snprintf(page_buf, sizeof page_buf, "Page %d", ui->page);
    y += 14;
    a = cn_text_set_size(t, 24);
    if (a > 0)
        y += a;
    cn_text_render(t, c, page_buf, &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    y += 14;
    a = cn_text_set_size(t, 24);
    if (a > 0)
        y += a;
    cn_text_render(t, c, CYR_SAMPLE, &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    /* footer hints */
    a = cn_text_set_size(t, 14);
    if (a > 0)
        y += a;
    {
        int fy = cn_canvas_height(c) - a - 4;
        (void)cn_text_render(t, c, "NEXT/PREV page  BACK/HOME home  PWR>2s exit",
                             &fy, MARGIN_L, MARGIN_R, CN_COLOR_GRAY,
                             CN_COLOR_WHITE);
    }

    draw_marker(c, ui);
}

static void render_library_rows(cn_ui *ui, cn_canvas *c, cn_text *t,
                                int asc)
{
    int rows = cn_ui_lib_rows();
    int r;
    int w = cn_canvas_width(c);

    for (r = 0; r < rows; r++) {
        int idx = ui->top + r;
        const cn_book *b;
        int ry0 = CN_UI_LIB_ROW_TOP + r * CN_UI_LIB_ROW_H;
        int base = ry0 + 6 + (asc > 0 ? asc : 0);
        uint16_t bg = CN_COLOR_WHITE;

        if (!ui->lib || idx >= ui->lib->count)
            break;
        b = &ui->lib->books[idx];
        if (idx == ui->sel) {
            cn_canvas_fill_rect(c, MARGIN_L, ry0,
                                w - MARGIN_R - 1, ry0 + CN_UI_LIB_ROW_H - 1,
                                CN_COLOR_GRAY);
            bg = CN_COLOR_GRAY;
        }
        render_fit(t, c, b->title, base, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, bg);
    }
}

static void render_library(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    int y = MARGIN_TOP;
    int a;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    a = cn_text_set_size(t, 48);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "CrossNook", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    y += 14;
    a = cn_text_set_size(t, 24);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "Library", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    if (!ui->lib || ui->lib->count == 0) {
        y += 18;
        a = cn_text_set_size(t, 24);
        if (a > 0)
            y += a;
        cn_text_render(t, c, "No books found.", &y, MARGIN_L, MARGIN_R,
                       CN_COLOR_BLACK, CN_COLOR_WHITE);
        return;
    }

    a = cn_text_set_size(t, 24);
    render_library_rows(ui, c, t, a);

    {
        char footer[64];
        int rows = cn_ui_lib_rows();
        int last = ui->top + rows;
        int fy;
        if (last > ui->lib->count)
            last = ui->lib->count;
        snprintf(footer, sizeof footer, "%d\xe2\x80\x93%d of %d",
                 ui->top + 1, last, ui->lib->count);
        (void)cn_text_set_size(t, 14);
        fy = CN_UI_LIB_FOOTER_Y;
        (void)cn_text_render(t, c, footer, &fy, MARGIN_L, MARGIN_R,
                             CN_COLOR_GRAY, CN_COLOR_WHITE);
    }
}

static void render_selected(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    const cn_book *b = cn_ui_selected_book(ui);
    int y = MARGIN_TOP;
    int a;

    cn_canvas_clear(c, CN_COLOR_WHITE);

    a = cn_text_set_size(t, 36);
    if (a > 0)
        y += a;
    cn_text_render(t, c, "Selected book", &y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

    if (b) {
        y += 16;
        a = cn_text_set_size(t, 48);
        if (a > 0)
            y += a;
        render_fit(t, c, b->title, y, MARGIN_L, MARGIN_R,
                   CN_COLOR_BLACK, CN_COLOR_WHITE);

        y += 14;
        a = cn_text_set_size(t, 24);
        if (a > 0)
            y += a;
        cn_text_render(t, c, cn_book_format_name(b->format),
                       &y, MARGIN_L, MARGIN_R,
                       CN_COLOR_BLACK, CN_COLOR_WHITE);

        y += 14;
        a = cn_text_set_size(t, 20);
        if (a > 0)
            y += a;
        render_fit(t, c, b->path, y, MARGIN_L, MARGIN_R,
                   CN_COLOR_GRAY, CN_COLOR_WHITE);
    }

    /* "[Reader not implemented]" note box spanning the content column */
    cn_canvas_outline_rect(c, BTN_X0, 440,
                           cn_canvas_width(c) - MARGIN_R - 1, 482,
                           CN_COLOR_BLACK);
    a = cn_text_set_size(t, 24);
    {
        int yy = 440 + (482 - 440) / 2 + (a > 0 ? a : 0) / 2 - 6;
        (void)cn_text_render(t, c, "[Reader not implemented]", &yy,
                             MARGIN_L + 8, MARGIN_R, CN_COLOR_GRAY,
                             CN_COLOR_WHITE);
    }
}

static void render_book(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    (void)t;   /* book pages are CREngine-rendered, not text-module drawn */
    if (ui->reader && cn_reader_is_open(ui->reader)) {
        (void)cn_reader_render(ui->reader, cn_canvas_pixels(c),
                               cn_canvas_width(c), cn_canvas_height(c));
        return;      /* full-screen page; no diagnostic touch marker */
    }
    cn_canvas_clear(c, CN_COLOR_WHITE);
}

static void render_sync_modal(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    static const char *const choices[] = {
        "Use this device", "Use remote progress", "Cancel"
    };
    static const char *const feedback[] = {
        "Working...", "Remote updated", "Remote applied",
        "Remote progress missing", "Upload may have succeeded; check first",
        "Position changed; sync again", "Disk/Reader mismatch; save blocked",
        "Sync choice failed; check and retry manually"
    };
    int y, i;
    cn_canvas_fill_rect(c, MARGIN_L, 176, CN_READER_W - MARGIN_R - 1,
                        552, CN_COLOR_WHITE);
    cn_canvas_outline_rect(c, MARGIN_L, 176, CN_READER_W - MARGIN_R - 1,
                           552, CN_COLOR_BLACK);
    if (!t) return;
    (void)cn_text_set_size(t, 24);
    y = 230;
    (void)cn_text_render(t, c, ui->sync_modal == 1 ? "Sync conflict" : "Sync result",
                         &y, MARGIN_L + 16, MARGIN_R + 16,
                         CN_COLOR_BLACK, CN_COLOR_WHITE);
    if (ui->sync_modal == 2) {
        y = 325;
        (void)cn_text_render(t, c, feedback[ui->sync_feedback], &y,
                             MARGIN_L + 16, MARGIN_R + 16,
                             CN_COLOR_BLACK, CN_COLOR_WHITE);
        y = 490;
        (void)cn_text_render(t, c, "BACK/HOME: return to Reader", &y,
                             MARGIN_L + 16, MARGIN_R + 16,
                             CN_COLOR_BLACK, CN_COLOR_WHITE);
        return;
    }
    for (i = 0; i < 3; ++i) {
        int top = SYNC_ROW_TOP + i * SYNC_ROW_H;
        uint16_t bg = ui->sync_selection == i ? CN_COLOR_GRAY : CN_COLOR_WHITE;
        if (ui->sync_selection == i)
            cn_canvas_fill_rect(c, MARGIN_L + 8, top,
                                CN_READER_W - MARGIN_R - 9,
                                top + SYNC_ROW_H - 2, bg);
        y = top + 39;
        (void)cn_text_render(t, c, choices[i], &y, MARGIN_L + 24,
                             MARGIN_R + 24, CN_COLOR_BLACK, bg);
    }
    y = 513;
    (void)cn_text_set_size(t, 16);
    (void)cn_text_render(t, c, "NEXT/PREV: select  MENU: confirm  BACK: cancel",
                         &y, MARGIN_L + 16, MARGIN_R + 16,
                         CN_COLOR_BLACK, CN_COLOR_WHITE);
}

void cn_ui_render(cn_ui *ui, cn_canvas *c, cn_text *t)
{
    if (!ui || !c) return;
    switch (ui->state) {
    case CN_UI_ACCOUNT_SETUP:  render_account(ui, c, t);         break;
    case CN_UI_LIBRARY:        render_library(ui, c, t);         break;
    case CN_UI_SELECTED_BOOK:  render_selected(ui, c, t);        break;
    case CN_UI_READER:         render_book(ui, c, t);            break;
    case CN_UI_READER_TEST:    render_reader(ui, c, t);          break;
    case CN_UI_HOME:
    default:                   render_home(ui, c, t);            break;
    }
    if (ui->state == CN_UI_READER && ui->sync_modal)
        render_sync_modal(ui, c, t);
}

int cn_ui_exit_requested(const cn_ui *ui)
{
    return ui->exit_requested;
}

cn_ui_state cn_ui_get_state(const cn_ui *ui)
{
    return ui->state;
}

int cn_ui_page(const cn_ui *ui)
{
    return ui->page;
}

const cn_library *cn_ui_library(const cn_ui *ui)
{
    return ui->lib;
}

int cn_ui_selection(const cn_ui *ui)
{
    return ui->sel;
}

int cn_ui_viewport(const cn_ui *ui)
{
    return ui->top;
}

const cn_book *cn_ui_selected_book(const cn_ui *ui)
{
    if (!ui->lib || ui->sel < 0 || ui->sel >= ui->lib->count)
        return NULL;
    return &ui->lib->books[ui->sel];
}

const char *cn_ui_state_name(cn_ui_state s)
{
    switch (s) {
    case CN_UI_HOME:          return "HOME";
    case CN_UI_READER_TEST:   return "READER";
    case CN_UI_LIBRARY:       return "LIBRARY";
    case CN_UI_SELECTED_BOOK: return "SELECTED";
    case CN_UI_READER:        return "READER";
    case CN_UI_ACCOUNT_SETUP: return "ACCOUNT_SETUP";
    default:                  return "?";
    }
}

int cn_ui_reader_pages(const cn_ui *ui)
{
    return (ui && ui->reader) ? cn_reader_pages(ui->reader) : 0;
}

int cn_ui_reader_page(const cn_ui *ui)
{
    return (ui && ui->reader) ? cn_reader_page(ui->reader) : 0;
}

int cn_ui_reader_get_position(cn_ui *ui, cn_reader_position *position)
{
    if (!ui || !ui->reader)
        return -1;
    return cn_reader_get_position(ui->reader, position);
}

int cn_ui_reader_goto_position(cn_ui *ui,
                               const cn_reader_position *position)
{
    if (!ui || !ui->reader)
        return -1;
    return cn_reader_goto_position(ui->reader, position);
}
