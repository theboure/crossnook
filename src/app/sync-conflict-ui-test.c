/* Bounded host matrix and verified-card Reader conflict UI diagnostic. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/reader_conflict.h"
#include "platform/nook/display.h"
#include "platform/storage_verify.h"
#include "storage/storage_layout.h"

#define MOCK_KEY "dfb450efddbb5387197c84460623675b"
#define SYNTHETIC_ACCOUNT "integration-different"
#define RUNTIME_ID "synthetic-conflict-runtime-id"

static int fake, failures, normal_calls, push_calls, pull_calls, save_calls;
static int get_calls, put_calls, apply_calls, fail_save, fail_apply;
static cn_kosync_product_outcome normal_outcome;
static cn_kosync_sync_decision normal_decision;
static cn_sync_pull_outcome pull_outcome;
static cn_sync_push_outcome push_outcome;
static cn_kosync_mutation_state push_mutation;
static const char *current_remote;
static char server_position[CN_READER_POSITION_MAX_BYTES + 1];
static cn_ui_sync_feedback feedback_for(const cn_reader_conflict_result *result);

cn_sync_controller_result __real_cn_sync_current_book(const cn_sync_controller_config *);
cn_sync_push_result __real_cn_sync_push_local_current_book(const cn_sync_controller_config *);
cn_sync_pull_result __real_cn_sync_pull_remote_current_book(const cn_sync_controller_config *);
cn_progress_result __real_cn_progress_store_save(cn_progress_store *, const cn_book_identity *, const cn_progress_record *);
int __real_cn_ui_reader_goto_position(cn_ui *, const cn_reader_position *);

static int record_equals(cn_progress_store *store, const char *book,
                         const char *position)
{
    cn_book_identity id;
    cn_progress_record record;
    int equal;
    if (cn_book_identity_from_path(&id, book) != 0) return 0;
    cn_progress_record_init(&record);
    equal = cn_progress_store_load(store, &id, &record) == CN_PROGRESS_OK &&
            record.position.location && position &&
            !strcmp(record.position.location, position);
    cn_progress_record_clear(&record);
    return equal;
}
static int seed(cn_progress_store *store, const char *book,
                const char *position, int percentage)
{
    cn_book_identity id;
    cn_progress_record record;
    cn_progress_result saved;
    if (cn_book_identity_from_path(&id, book) != 0) return 0;
    cn_progress_record_init(&record);
    record.position.location = (char *)position;
    record.position.progress_10000 = percentage;
    saved = __real_cn_progress_store_save(store, &id, &record);
    record.position.location = NULL;
    return saved == CN_PROGRESS_OK;
}

cn_sync_controller_result __wrap_cn_sync_current_book(const cn_sync_controller_config *config)
{
    cn_sync_controller_result result;
    if (!fake) return __real_cn_sync_current_book(config);
    ++normal_calls;
    memset(&result, 0, sizeof result);
    result.stage = CN_SYNC_CONTROLLER_INTEGRATION;
    result.sync_status = CN_KOSYNC_SYNC_STATUS_OK;
    result.decision = normal_decision;
    result.product.outcome = normal_outcome;
    return result;
}
cn_sync_push_result __wrap_cn_sync_push_local_current_book(const cn_sync_controller_config *config)
{
    cn_sync_push_result result;
    cn_book_identity id;
    cn_progress_record record;
    if (!fake) return __real_cn_sync_push_local_current_book(config);
    ++push_calls;
    memset(&result, 0, sizeof result);
    result.stage = CN_SYNC_PUSH_EXECUTED;
    result.outcome = push_outcome;
    result.remote_mutation = push_mutation;
    if (!config->device_id || strcmp(config->device_id, RUNTIME_ID)) {
        result.outcome = CN_SYNC_PUSH_CONFIGURATION_FAILURE;
        result.remote_mutation = CN_KOSYNC_MUTATION_NONE;
        return result;
    }
    if (push_outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME ||
        push_mutation == CN_KOSYNC_MUTATION_POSSIBLE) {
        ++put_calls;
        if (cn_book_identity_from_path(&id, config->document_path) != 0) ++failures;
        else {
            cn_progress_record_init(&record);
            if (cn_progress_store_load(config->progress_store, &id, &record) != CN_PROGRESS_OK ||
                !record.position.location) ++failures;
            else if (strlen(record.position.location) < sizeof server_position)
                strcpy(server_position, record.position.location);
            else ++failures;
            cn_progress_record_clear(&record);
        }
    }
    return result;
}
cn_sync_pull_result __wrap_cn_sync_pull_remote_current_book(const cn_sync_controller_config *config)
{
    cn_sync_pull_result result;
    if (!fake) return __real_cn_sync_pull_remote_current_book(config);
    ++pull_calls; ++get_calls;
    memset(&result, 0, sizeof result);
    result.stage = CN_SYNC_PULL_EXECUTED;
    result.outcome = pull_outcome;
    if (pull_outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
        (!current_remote || !seed(config->progress_store,
                                  config->document_path, current_remote, 4321)))
        ++failures;
    return result;
}
cn_progress_result __wrap_cn_progress_store_save(
    cn_progress_store *store, const cn_book_identity *id,
    const cn_progress_record *record)
{
    if (fake) {
        ++save_calls;
        if (fail_save) return CN_PROGRESS_IO_ERROR;
    }
    return __real_cn_progress_store_save(store, id, record);
}
int __wrap_cn_ui_reader_goto_position(cn_ui *ui, const cn_reader_position *position)
{
    if (fake) {
        ++apply_calls;
        if (fail_apply) return -1;
    }
    return __real_cn_ui_reader_goto_position(ui, position);
}

static void check(int condition, const char *description)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", description);
    fflush(stdout);
    if (!condition) ++failures;
}
static void reset_fake(void)
{
    fake = 1;
    normal_calls = push_calls = pull_calls = save_calls = get_calls = put_calls = 0;
    apply_calls = fail_save = fail_apply = 0;
    normal_outcome = CN_KOSYNC_PRODUCT_CONFLICT;
    normal_decision = CN_KOSYNC_SYNC_AMBIGUOUS;
    pull_outcome = CN_SYNC_PULL_PERSISTED_OUTCOME;
    push_outcome = CN_SYNC_PUSH_UPLOADED_OUTCOME;
    push_mutation = CN_KOSYNC_MUTATION_CONFIRMED;
}
static void key(cn_ui *ui, cn_input_event type)
{
    cn_input_ev event;
    memset(&event, 0, sizeof event);
    event.type = type;
    (void)cn_ui_handle(ui, &event);
}
static int reader_is(cn_ui *ui, const char *position)
{
    cn_reader_position current;
    int same;
    cn_reader_position_init(&current);
    same = cn_ui_reader_get_position(ui, &current) == 0 &&
           current.location && position && !strcmp(current.location, position);
    cn_reader_position_clear(&current);
    return same;
}
static cn_ui *open_reader(const char *font, const char *book, cn_library **library)
{
    cn_reader_config config;
    cn_ui *ui = cn_ui_init();
    *library = cn_library_new();
    memset(&config, 0, sizeof config); config.font_path = font;
    if (!ui || !*library ||
        cn_library_add(*library, book, "synthetic.epub", CN_BOOK_EPUB) < 0 ||
        cn_ui_set_reader(ui, &config) != 0 ||
        cn_ui_set_library(ui, *library) != 1) goto fail;
    {
        cn_input_ev event;
        memset(&event, 0, sizeof event);
        event.type = CN_INPUT_TOUCH_UP; event.x = 200; event.y = 320;
        (void)cn_ui_handle(ui, &event);
        event.y = CN_UI_LIB_ROW_TOP + 10;
        (void)cn_ui_handle(ui, &event);
    }
    if (cn_ui_get_state(ui) == CN_UI_READER) return ui;
fail:
    cn_ui_free(ui);
    cn_library_free(*library);
    *library = NULL;
    return NULL;
}
static cn_reader_sync_result manual(cn_ui *ui, cn_progress_store *store,
                                    const cn_sync_controller_config *controller)
{
    cn_reader_sync_config config;
    config.ui = ui; config.progress_store = store; config.controller = controller;
    return cn_reader_sync_manual_once(&config);
}

static int smoke(const char *font, const char *book, const char *directory)
{
    cn_progress_store *store = NULL;
    cn_library *library = NULL;
    cn_ui *ui;
    cn_canvas *canvas = NULL;
    cn_text *text = NULL;
    cn_reader_position a, b, c, snapshot;
    cn_reader_sync_result original, other;
    cn_reader_conflict_result resolution;
    cn_reader_sync_config config;
    cn_sync_controller_config controller;
    cn_input_ev touch;
    char *path = NULL;
    int i, before;
    static const cn_kosync_product_outcome others[] = {
        CN_KOSYNC_PRODUCT_UPLOADED, CN_KOSYNC_PRODUCT_UNCHANGED,
        CN_KOSYNC_PRODUCT_DISABLED, CN_KOSYNC_PRODUCT_AUTH_REQUIRED,
        CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE,
        CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE,
        CN_KOSYNC_PRODUCT_SECURITY_FAILURE,
        CN_KOSYNC_PRODUCT_SERVICE_FAILURE
    };
    cn_reader_position_init(&a); cn_reader_position_init(&b);
    cn_reader_position_init(&c);
    cn_reader_position_init(&snapshot);
    if (cn_progress_store_open(&store, directory) != CN_PROGRESS_OK) return 1;
    ui = open_reader(font, book, &library);
    text = cn_text_load(font); canvas = cn_canvas_create(CN_READER_W, CN_READER_H);
    if (!ui || !text || !canvas) return 1;
    for (i = 0; i < 5; ++i) key(ui, CN_INPUT_PAGE_NEXT);
    if (cn_ui_reader_get_position(ui, &a) != 0 || !a.location) return 1;
    for (i = 0; i < 4; ++i) key(ui, CN_INPUT_PAGE_PREV);
    if (cn_ui_reader_get_position(ui, &b) != 0 || !b.location ||
        !strcmp(a.location, b.location) || cn_ui_reader_goto_position(ui, &a) != 0)
        return 1;
    for (i = 0; i < 2; ++i) key(ui, CN_INPUT_PAGE_PREV);
    if (cn_ui_reader_get_position(ui, &c) != 0 || !c.location ||
        !strcmp(c.location, a.location) || !strcmp(c.location, b.location) ||
        cn_ui_reader_goto_position(ui, &a) != 0) return 1;
    path = strdup(book);
    if (!path || cn_reader_position_copy(&snapshot, &a) != 0) return 1;
    memset(&controller, 0, sizeof controller);
    controller.device_id = RUNTIME_ID;
    config.ui = ui; config.progress_store = store; config.controller = &controller;
    reset_fake();
    fail_save = 1;
    other = manual(ui, store, &controller);
    check(other.phase == CN_READER_SYNC_PRE_SAVE_FAILED &&
          !cn_reader_sync_is_conflict(&other) && !normal_calls &&
          !cn_ui_sync_modal_active(ui),
          "failed initial pre-save never runs normal controller or conflict UI");
    reset_fake();
    current_remote = b.location;
    original = manual(ui, store, &controller);
    check(cn_reader_sync_is_conflict(&original) && normal_calls == 1 &&
          save_calls == 1 && record_equals(store, book, a.location) &&
          reader_is(ui, a.location),
          "save-first normal conflict leaves persisted A and Reader A");
    check(cn_ui_show_sync_conflict(ui) == 0 &&
          cn_ui_get_state(ui) == CN_UI_READER &&
          cn_ui_sync_selection(ui) == 2 && !push_calls && !pull_calls,
          "only proven conflict presents modal choices, Cancel initially selected");
    cn_text_reset_stats(text);
    cn_ui_render(ui, canvas, text);
    check(cn_text_get_stats(text).ink > 0,
          "conflict choices render visible text over the real Reader");
    key(ui, CN_INPUT_PAGE_PREV);
    check(cn_ui_sync_selection(ui) == 1 && reader_is(ui, a.location),
          "PREV selects remote without moving Reader");
    key(ui, CN_INPUT_PAGE_NEXT);
    check(cn_ui_sync_selection(ui) == 2 && reader_is(ui, a.location),
          "NEXT selects Cancel without moving Reader");
    memset(&touch, 0, sizeof touch); touch.type = CN_INPUT_TOUCH_UP;
    touch.x = 200; touch.y = 282;
    (void)cn_ui_handle(ui, &touch);
    check(cn_ui_sync_selection(ui) == 0 && !push_calls && !pull_calls,
          "touch highlights local but cannot authorize upload");
    key(ui, CN_INPUT_BACK);
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                  &snapshot, path, CN_READER_CONFLICT_CANCEL);
    check(cn_ui_take_sync_action(ui) == CN_UI_SYNC_CANCEL &&
          resolution.phase == CN_READER_CONFLICT_CANCELLED &&
          !cn_ui_sync_modal_active(ui) && cn_ui_get_state(ui) == CN_UI_READER &&
          !push_calls && !pull_calls && record_equals(store, book, a.location) &&
          reader_is(ui, a.location), "BACK cancel: no resolution network/save/move");
    check(cn_ui_show_sync_conflict(ui) == 0, "reopen conflict for HOME cancellation");
    key(ui, CN_INPUT_HOME);
    check(cn_ui_take_sync_action(ui) == CN_UI_SYNC_CANCEL &&
          cn_ui_get_state(ui) == CN_UI_READER && !push_calls && !pull_calls,
          "HOME dismisses modal without Reader close or network");
    check(cn_ui_show_sync_conflict(ui) == 0, "reopen conflict for MENU Cancel");
    key(ui, CN_INPUT_MENU);
    check(cn_ui_take_sync_action(ui) == CN_UI_SYNC_CANCEL &&
          !cn_ui_sync_modal_active(ui) && reader_is(ui, a.location) &&
          record_equals(store, book, a.location) && !push_calls && !pull_calls,
          "MENU on default Cancel dismisses without mutation or working overlay");
    for (i = 0; i < (int)(sizeof others / sizeof others[0]); ++i) {
        reset_fake(); normal_outcome = others[i];
        normal_decision = CN_KOSYNC_SYNC_NO_CHANGE;
        other = manual(ui, store, &controller);
        if (cn_reader_sync_is_conflict(&other) || cn_ui_sync_modal_active(ui))
            ++failures;
    }
    check(!cn_ui_sync_modal_active(ui),
          "uploaded/unchanged/disabled/auth/config/network/security/service do not offer choices");
    reset_fake(); normal_outcome = CN_KOSYNC_PRODUCT_CONFLICT;
    normal_decision = CN_KOSYNC_SYNC_NO_CHANGE;
    other = manual(ui, store, &controller);
    check(!cn_reader_sync_is_conflict(&other),
          "conflict label without AMBIGUOUS decision is ineligible");

    reset_fake();
    check(cn_ui_show_sync_conflict(ui) == 0, "local choice overlay open");
    key(ui, CN_INPUT_PAGE_PREV); key(ui, CN_INPUT_PAGE_PREV);
    key(ui, CN_INPUT_MENU);
    check(cn_ui_take_sync_action(ui) == CN_UI_SYNC_USE_LOCAL &&
          cn_ui_sync_modal_active(ui), "MENU authorizes local exactly once");
    key(ui, CN_INPUT_MENU);
    check(cn_ui_take_sync_action(ui) == CN_UI_SYNC_NONE,
          "repeated MENU cannot emit another destructive action");
    before = save_calls;
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_LOCAL);
    check(resolution.phase == CN_READER_CONFLICT_LOCAL_COMPLETE &&
          resolution.push_invoked && !resolution.pull_invoked &&
          resolution.push.remote_mutation == CN_KOSYNC_MUTATION_CONFIRMED &&
          !strcmp(server_position, a.location) && put_calls == 1 && !get_calls &&
          save_calls == before && reader_is(ui, a.location) &&
          record_equals(store, book, a.location),
          "local choice uploads persisted A once, no GET or second save");
    (void)cn_ui_show_sync_feedback(ui, CN_UI_SYNC_FEEDBACK_REMOTE_UPDATED);
    key(ui, CN_INPUT_MENU);
    check(cn_ui_take_sync_action(ui) == CN_UI_SYNC_NONE,
          "acknowledgement consumes repeated MENU");
    key(ui, CN_INPUT_BACK);

    reset_fake(); push_outcome = CN_SYNC_PUSH_LOCAL_UNSUPPORTED_OUTCOME;
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_LOCAL);
    check(resolution.push.outcome == CN_SYNC_PUSH_LOCAL_UNSUPPORTED_OUTCOME &&
          !put_calls && !get_calls, "unsupported local choice has no network");
    reset_fake(); push_outcome = CN_SYNC_PUSH_CONFIGURATION_FAILURE;
    controller.device_id = NULL;
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_LOCAL);
    check(resolution.push.outcome == CN_SYNC_PUSH_CONFIGURATION_FAILURE &&
          !put_calls, "invalid device ID feedback retains typed configuration failure");
    controller.device_id = RUNTIME_ID;

    reset_fake();
    (void)cn_ui_reader_goto_position(ui, &b);
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_LOCAL);
    check(resolution.phase == CN_READER_CONFLICT_STALE && !push_calls &&
          !pull_calls && record_equals(store, book, a.location),
          "Reader A->B invalidates conflict before network");
    (void)cn_ui_reader_goto_position(ui, &a);
    check(seed(store, book, b.location, b.progress_10000),
          "seed changed persisted position for stale check");
    reset_fake();
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_REMOTE);
    check(resolution.phase == CN_READER_CONFLICT_STALE && !pull_calls &&
          !push_calls && reader_is(ui, a.location),
          "persisted A->B invalidates conflict before GET");
    check(seed(store, book, a.location, a.progress_10000),
          "restore saved A after stale test");
    reset_fake();
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, "/tmp/different-book.epub", CN_READER_CONFLICT_USE_LOCAL);
    check(resolution.phase == CN_READER_CONFLICT_STALE && !push_calls &&
          !pull_calls, "changed exact path invalidates conflict");

    reset_fake(); current_remote = c.location;
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_REMOTE);
    check(resolution.phase == CN_READER_CONFLICT_REMOTE_APPLIED &&
          resolution.pull_invoked && !resolution.push_invoked &&
          resolution.redraw_needed && pull_calls == 1 && get_calls == 1 &&
          !put_calls && apply_calls == 1 &&
          record_equals(store, book, c.location) && reader_is(ui, c.location) &&
          strcmp(c.location, b.location),
          "fresh remote C persisted before separate Reader apply, GET=1 PUT=0");
    cn_ui_render(ui, canvas, text);
    (void)cn_ui_reader_goto_position(ui, &a);
    check(seed(store, book, a.location, a.progress_10000), "restore A for missing remote");
    reset_fake(); pull_outcome = CN_SYNC_PULL_REMOTE_MISSING_OUTCOME;
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_REMOTE);
    check(resolution.phase == CN_READER_CONFLICT_REMOTE_COMPLETE &&
          resolution.pull.outcome == CN_SYNC_PULL_REMOTE_MISSING_OUTCOME &&
          pull_calls == 1 && get_calls == 1 && !apply_calls && !put_calls &&
          record_equals(store, book, a.location) && reader_is(ui, a.location),
          "fresh remote missing leaves local/Reader A, no apply");
    {
        static const cn_sync_pull_outcome errors[] = {
            CN_SYNC_PULL_AUTH_REQUIRED, CN_SYNC_PULL_CONNECTIVITY_FAILURE,
            CN_SYNC_PULL_SECURITY_FAILURE, CN_SYNC_PULL_SERVICE_FAILURE
        };
        for (i = 0; i < (int)(sizeof errors / sizeof errors[0]); ++i) {
            reset_fake(); pull_outcome = errors[i];
            resolution = cn_reader_sync_resolve_conflict(&config, &original,
                         &snapshot, path, CN_READER_CONFLICT_USE_REMOTE);
            if (resolution.phase != CN_READER_CONFLICT_REMOTE_COMPLETE ||
                resolution.pull.outcome != errors[i] || apply_calls || put_calls ||
                !record_equals(store, book, a.location) || !reader_is(ui, a.location))
                ++failures;
        }
        check(!failures, "remote auth/network/TLS/service failures never apply Reader");
    }
    reset_fake(); current_remote = b.location; fail_apply = 1;
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_REMOTE);
    check(resolution.phase == CN_READER_CONFLICT_REMOTE_APPLY_FAILED &&
          resolution.disk_reader_mismatch_possible &&
          record_equals(store, book, b.location) && reader_is(ui, a.location),
          "persisted B / failed apply retains disk B and Reader A mismatch");
    (void)cn_ui_show_sync_feedback(ui, CN_UI_SYNC_FEEDBACK_MISMATCH);
    key(ui, CN_INPUT_HOME);
    check(resolution.disk_reader_mismatch_possible &&
          !cn_ui_sync_modal_active(ui) && cn_ui_get_state(ui) == CN_UI_READER &&
          !save_calls && record_equals(store, book, b.location),
          "mismatch acknowledgement HOME leaves disk B; no Reader A exit-save");
    (void)cn_ui_reader_goto_position(ui, &a);
    check(seed(store, book, a.location, a.progress_10000), "restore A for uncertain PUT");
    reset_fake(); push_outcome = CN_SYNC_PUSH_CONNECTIVITY_FAILURE;
    push_mutation = CN_KOSYNC_MUTATION_POSSIBLE;
    resolution = cn_reader_sync_resolve_conflict(&config, &original,
                 &snapshot, path, CN_READER_CONFLICT_USE_LOCAL);
    check(resolution.phase == CN_READER_CONFLICT_LOCAL_COMPLETE &&
          resolution.push.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE &&
          put_calls == 1 && !get_calls && record_equals(store, book, a.location) &&
          feedback_for(&resolution) == CN_UI_SYNC_FEEDBACK_UPLOAD_POSSIBLE,
          "applied PUT/lost response stays POSSIBLE with no retry");
    (void)cn_ui_show_sync_feedback(ui, CN_UI_SYNC_FEEDBACK_UPLOAD_POSSIBLE);
    cn_ui_render(ui, canvas, text);
    key(ui, CN_INPUT_MENU);
    check(cn_ui_take_sync_action(ui) == CN_UI_SYNC_NONE,
          "uncertain upload feedback cannot issue automatic repeat");

    free(path);
    cn_reader_position_clear(&snapshot); cn_reader_position_clear(&a);
    cn_reader_position_clear(&b); cn_reader_position_clear(&c);
    cn_ui_free(ui); cn_library_free(library);
    cn_canvas_free(canvas); cn_text_free(text);
    cn_progress_store_close(store);
    fake = 0;
    printf("SYNC CONFLICT UI SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int number(const char *text, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long n;
    if (!text || !*text || *text == '-') return 0;
    errno = 0; n = strtoul(text, &end, 10);
    if (errno || *end || n > max) return 0;
    *out = n;
    return 1;
}
static int same_device_dir(const char *path, const struct stat *root)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && st.st_dev == root->st_dev;
}
static cn_ui_sync_feedback feedback_for(const cn_reader_conflict_result *result)
{
    if (result->phase == CN_READER_CONFLICT_STALE)
        return CN_UI_SYNC_FEEDBACK_STALE;
    if (result->phase == CN_READER_CONFLICT_REMOTE_APPLY_FAILED)
        return CN_UI_SYNC_FEEDBACK_MISMATCH;
    if (result->phase == CN_READER_CONFLICT_REMOTE_APPLIED)
        return CN_UI_SYNC_FEEDBACK_REMOTE_APPLIED;
    if (result->phase == CN_READER_CONFLICT_LOCAL_COMPLETE) {
        if (result->push.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE)
            return CN_UI_SYNC_FEEDBACK_UPLOAD_POSSIBLE;
        if (result->push.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME &&
            result->push.remote_mutation == CN_KOSYNC_MUTATION_CONFIRMED)
            return CN_UI_SYNC_FEEDBACK_REMOTE_UPDATED;
    }
    if (result->phase == CN_READER_CONFLICT_REMOTE_COMPLETE &&
        result->pull.outcome == CN_SYNC_PULL_REMOTE_MISSING_OUTCOME)
        return CN_UI_SYNC_FEEDBACK_REMOTE_MISSING;
    return CN_UI_SYNC_FEEDBACK_FAILED;
}

/* A verified-card device app, separate from the unverified reader-test demo.
 * Setup PUT and verification GET are isolated processes/transcript phases. */
static int physical(int argc, char **argv)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_progress_store *store = NULL;
    cn_settings settings;
    cn_credentials credentials;
    cn_dns_config dns;
    cn_time_config time;
    cn_tls_config tls;
    cn_sync_controller_config controller;
    cn_reader_sync_config config;
    cn_reader_sync_result original;
    cn_reader_conflict_result resolution;
    cn_kosync_client client, routed;
    cn_dns_answer answer;
    cn_time_sample sample;
    cn_koreader_document_id remote_id;
    cn_kosync_progress upload, remote;
    cn_kosync_outcome outcome;
    cn_book_identity local_id;
    cn_progress_record persisted;
    cn_reader_position a, b, snapshot;
    cn_library *library = NULL;
    cn_ui *ui = NULL;
    cn_text *text = NULL;
    cn_canvas *canvas = NULL;
    cn_display *display = NULL;
    cn_input *input = NULL;
    cn_input_ev event;
    struct stat root_st;
    char gate[CN_STORAGE_PATH_CAPACITY], config_dir[CN_STORAGE_PATH_CAPACITY];
    char state_dir[CN_STORAGE_PATH_CAPACITY], progress_dir[CN_STORAGE_PATH_CAPACITY];
    char *exact_path = NULL;
    unsigned long major_number, minor_number, dns_port, time_port;
    const char *mode;
    int rc = 1, saved_stderr = -1, sink = -1;
    int mismatch = 0, finished = 0, resolved = 0;
    int redraw, reader_equal, local_equal;

    if (argc != 15) return 2;
    mode = argv[2];
    if (strcmp(mode, "seed-remote") && strcmp(mode, "run") &&
        strcmp(mode, "verify-remote")) return 2;
    if (!number(argv[6], 0xffffffffUL, &major_number) ||
        !number(argv[7], 0xffffffffUL, &minor_number) ||
        !number(argv[11], 65535, &dns_port) || !dns_port ||
        !number(argv[13], 65535, &time_port) || !time_port) return 2;
    candidate.mountpoint = argv[4]; candidate.root = argv[5];
    candidate.expected_major = (unsigned)major_number;
    candidate.expected_minor = (unsigned)minor_number;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) != CN_PLATFORM_STORAGE_OK) {
        puts("SYNC CONFLICT GATE storage=unverified persistence=not-attempted network=not-attempted");
        return 1;
    }
    if (stat(verified.root, &root_st) != 0 ||
        snprintf(gate, sizeof gate, "%s/controller-conflict", verified.root) >=
            (int)sizeof gate || !same_device_dir(gate, &root_st) ||
        cn_storage_layout_init(&layout, gate, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG,
                               config_dir, sizeof config_dir) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                               progress_dir, sizeof progress_dir) != CN_STORAGE_OK ||
        snprintf(state_dir, sizeof state_dir, "%s/state", gate) >=
            (int)sizeof state_dir || !same_device_dir(config_dir, &root_st) ||
        !same_device_dir(progress_dir, &root_st) ||
        !same_device_dir(state_dir, &root_st) ||
        cn_settings_store_init(&settings_store, config_dir, NULL) != CN_SETTINGS_OK ||
        cn_credential_store_init(&credential_store, state_dir, NULL) != CN_CREDENTIAL_OK ||
        cn_progress_store_open(&store, progress_dir) != CN_PROGRESS_OK) return 1;

    cn_reader_position_init(&a); cn_reader_position_init(&b);
    cn_reader_position_init(&snapshot); cn_progress_record_init(&persisted);
    cn_kosync_progress_init(&upload); cn_kosync_progress_init(&remote);
    memset(&credentials, 0, sizeof credentials);
    memset(&client, 0, sizeof client); memset(&routed, 0, sizeof routed);
    saved_stderr = dup(STDERR_FILENO); sink = open("/dev/null", O_WRONLY);
    if (saved_stderr < 0 || sink < 0 || dup2(sink, STDERR_FILENO) < 0) goto done;
    close(sink); sink = -1;
    if (cn_settings_load(&settings_store, &settings, NULL) != CN_SETTINGS_OK ||
        !settings.kosync_enabled ||
        cn_credential_store_load(&credential_store, &credentials, NULL) != CN_CREDENTIAL_OK ||
        strcmp(credentials.username, SYNTHETIC_ACCOUNT) ||
        strcmp(credentials.userkey, MOCK_KEY)) goto done;
    cn_credentials_clear(&credentials);
    ui = open_reader(argv[8], argv[9], &library);
    if (!ui || cn_book_identity_from_path(&local_id, argv[9]) != 0) goto done;
    for (redraw = 0; redraw < 5; ++redraw) key(ui, CN_INPUT_PAGE_NEXT);
    if (cn_ui_reader_get_position(ui, &a) != 0 || !a.location) goto done;
    exact_path = strdup(argv[9]);
    if (!exact_path) goto done;
    memset(&dns, 0, sizeof dns);
    dns.servers[0] = argv[10]; dns.server_count = 1; dns.port = (unsigned)dns_port;
    memset(&time, 0, sizeof time);
    time.servers[0] = argv[12]; time.server_count = 1; time.port = (unsigned)time_port;
    memset(&tls, 0, sizeof tls); tls.ca_path = argv[14];
    memset(&controller, 0, sizeof controller);
    controller.settings_store = &settings_store;
    controller.credential_store = &credential_store;
    controller.progress_store = store; controller.document_path = exact_path;
    controller.device_id = RUNTIME_ID;
    controller.dns = &dns; controller.time = &time; controller.tls = &tls;
    controller.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    config.ui = ui; config.progress_store = store; config.controller = &controller;

    if (!strcmp(mode, "run")) {
        text = cn_text_load(argv[8]); canvas = cn_canvas_create(CN_READER_W, CN_READER_H);
        display = cn_display_open(); input = cn_input_open();
        if (!text || !canvas || !display || !input) goto done;
        cn_ui_render(ui, canvas, text);
        if (cn_display_flush(display, cn_canvas_pixels(canvas)) != CN_FB_BYTES) goto done;
        puts("SYNC CONFLICT GATE reader=ready MENU=manual-sync");
        while (!finished) {
            int input_rc = cn_input_poll(input, &event, -1);
            if (input_rc < 0)
                break;
            if (input_rc == 0)
                continue;

            cn_ui_sync_action action;
            int had_modal = cn_ui_sync_modal_active(ui);
            redraw = cn_ui_handle(ui, &event);
            action = cn_ui_take_sync_action(ui);
            if (had_modal && (event.type == CN_INPUT_PAGE_NEXT ||
                              event.type == CN_INPUT_PAGE_PREV))
                printf("SYNC CONFLICT GATE selection=%d reader-unchanged=%s\n",
                       cn_ui_sync_selection(ui),
                       reader_is(ui, a.location) ? "yes" : "no");
            if (action == CN_UI_SYNC_MANUAL) {
                original = cn_reader_sync_manual_once(&config);
                printf("SYNC CONFLICT GATE normal-phase=%s normal-outcome=%s decision=%s\n",
                       cn_reader_sync_phase_name(original.phase),
                       cn_kosync_product_outcome_name(original.controller.product.outcome),
                       cn_kosync_sync_decision_name(original.controller.decision));
                if (!cn_reader_sync_is_conflict(&original)) {
                    /* Published automatic results need no conflict UI. */
                    if (original.phase == CN_READER_SYNC_CONTROLLER_COMPLETE &&
                        (original.controller.product.outcome == CN_KOSYNC_PRODUCT_UPLOADED ||
                         original.controller.product.outcome == CN_KOSYNC_PRODUCT_UNCHANGED ||
                         original.controller.product.outcome == CN_KOSYNC_PRODUCT_NO_STATE ||
                         original.controller.product.outcome == CN_KOSYNC_PRODUCT_DISABLED))
                        rc = 0;
                    else if (original.phase == CN_READER_SYNC_IMPORT_APPLIED)
                        rc = 0;
                    if (original.redraw_needed) {
                        cn_ui_render(ui, canvas, text);
                        if (cn_display_flush(display, cn_canvas_pixels(canvas)) != CN_FB_BYTES)
                            rc = 1;
                    }
                    goto done;
                }
                if (cn_reader_position_copy(&snapshot, &a) != 0 ||
                    cn_ui_show_sync_conflict(ui) != 0) goto done;
                redraw = 1;
                puts("SYNC CONFLICT GATE choices=3 selection=cancel reader-unchanged=yes");
            } else if (action == CN_UI_SYNC_CANCEL) {
                resolution = cn_reader_sync_resolve_conflict(&config, &original,
                              &snapshot, exact_path, CN_READER_CONFLICT_CANCEL);
                printf("SYNC CONFLICT GATE resolution=%s reader-unchanged=%s "
                       "network=not-attempted\n",
                       cn_reader_conflict_phase_name(resolution.phase),
                       reader_is(ui, a.location) ? "yes" : "no");
                finished = 1;
            } else if (action == CN_UI_SYNC_USE_LOCAL ||
                       action == CN_UI_SYNC_USE_REMOTE) {
                cn_platform_storage_verified again;
                if (cn_platform_storage_verify(&candidate, &again, NULL) !=
                        CN_PLATFORM_STORAGE_OK ||
                    strcmp(again.root, verified.root)) {
                    puts("SYNC CONFLICT GATE storage=unverified persistence=not-attempted network=not-attempted");
                    goto done;
                }
                resolution = cn_reader_sync_resolve_conflict(
                    &config, &original, &snapshot, exact_path,
                    action == CN_UI_SYNC_USE_LOCAL ? CN_READER_CONFLICT_USE_LOCAL :
                                                       CN_READER_CONFLICT_USE_REMOTE);
                mismatch = resolution.disk_reader_mismatch_possible;
                (void)cn_ui_show_sync_feedback(ui, feedback_for(&resolution));
                local_equal = record_equals(store, exact_path, a.location);
                reader_equal = reader_is(ui, a.location);
                printf("SYNC CONFLICT GATE resolution=%s local-outcome=%s "
                       "remote-outcome=%s remote-mutation=%s put-invoked=%d "
                       "get-attempted=%d local-still-a=%s reader-still-a=%s "
                       "redraw-needed=%d mismatch=%d exit-save=not-attempted\n",
                       cn_reader_conflict_phase_name(resolution.phase),
                       resolution.push_invoked
                           ? cn_sync_push_outcome_name(resolution.push.outcome)
                           : "not-attempted",
                       resolution.pull_invoked
                           ? cn_sync_pull_outcome_name(resolution.pull.outcome)
                           : "not-attempted",
                       cn_kosync_mutation_state_name(resolution.push.remote_mutation),
                       resolution.push.push.put_invoked,
                       resolution.pull.pull.get_attempted,
                       local_equal ? "yes" : "no", reader_equal ? "yes" : "no",
                       resolution.redraw_needed, mismatch);
                redraw = 1;
                resolved = 1;
            } else if (had_modal && event.type == CN_INPUT_BACK &&
                       !cn_ui_sync_modal_active(ui)) {
                /* Acknowledgement dismissal does not save an old Reader A. */
                redraw = 1;
            }
            if (resolved && had_modal &&
                (event.type == CN_INPUT_BACK || event.type == CN_INPUT_HOME) &&
                !cn_ui_sync_modal_active(ui)) {
                puts("SYNC CONFLICT GATE acknowledgement=dismissed exit-save=not-attempted");
                finished = 1;
            }
            if (redraw) {
                cn_ui_render(ui, canvas, text);
                if (cn_display_flush(display, cn_canvas_pixels(canvas)) != CN_FB_BYTES)
                    goto done;
                if (resolved && action == CN_UI_SYNC_USE_REMOTE &&
                    resolution.phase == CN_READER_CONFLICT_REMOTE_APPLIED) {
                    cn_progress_record confirmed;
                    int matched;
                    cn_progress_record_init(&confirmed);
                    matched = cn_progress_store_load(store, &local_id,
                                                     &confirmed) == CN_PROGRESS_OK &&
                              reader_is(ui, confirmed.position.location);
                    cn_progress_record_clear(&confirmed);
                    printf("SYNC CONFLICT GATE reader-matched-persisted=%s render=ok\n",
                           matched ? "yes" : "no");
                    if (!matched) goto done;
                }
            }
            if (cn_ui_exit_requested(ui)) break;
        }
        rc = finished && !mismatch ? 0 : 1;
        if (mismatch) puts("SYNC CONFLICT GATE mismatch=possible exit-save=not-attempted");
        goto done;
    }

    if (cn_kosync_client_init(&client, settings.kosync_base_url,
                              SYNTHETIC_ACCOUNT, MOCK_KEY) != CN_KOSYNC_OK ||
        !client.use_tls ||
        cn_book_identity_koreader_binary(exact_path, &remote_id) != CN_KOREADER_IDENTITY_OK ||
        cn_timesimple_sync(&time, &sample) != CN_TIME_OK ||
        cn_dnssimple_resolve_a(&dns, client.host, &answer) != CN_DNS_OK ||
        !answer.count) goto done;
    routed = client;
    if (cn_kosync_client_set_tls(&routed, &tls, answer.ipv4[0]) != CN_KOSYNC_OK)
        goto done;
    if (!strcmp(mode, "seed-remote")) {
        for (redraw = 0; redraw < 4; ++redraw) key(ui, CN_INPUT_PAGE_PREV);
        if (cn_ui_reader_get_position(ui, &b) != 0 || !b.location ||
            !strcmp(a.location, b.location) ||
            cn_kosync_progress_set(&upload,
                cn_koreader_document_id_text(&remote_id), b.location,
                b.progress_10000, "synthetic-seed-device", RUNTIME_ID) != CN_KOSYNC_OK ||
            cn_kosync_put_progress(&routed, &upload, NULL, &outcome) != CN_KOSYNC_OK)
            goto done;
        puts("SYNC CONFLICT GATE seed-remote=ok setup-put=1");
        rc = 0; goto done;
    }
    if (cn_kosync_get_progress(&routed, cn_koreader_document_id_text(&remote_id),
                               &remote, &outcome) != CN_KOSYNC_OK ||
        cn_progress_store_load(store, &local_id, &persisted) != CN_PROGRESS_OK ||
        !persisted.position.location || !remote.logical_position ||
        strcmp(persisted.position.location, remote.logical_position)) goto done;
    puts("SYNC CONFLICT GATE verify-remote=local-matched network=GET-only");
    rc = 0;
done:
    if (saved_stderr >= 0) {
        fflush(stderr); (void)dup2(saved_stderr, STDERR_FILENO); close(saved_stderr);
    }
    if (sink >= 0) close(sink);
    cn_input_close(input); cn_display_close(display);
    cn_canvas_free(canvas); cn_text_free(text);
    cn_kosync_progress_clear(&upload); cn_kosync_progress_clear(&remote);
    cn_credentials_clear(&credentials);
    memset(&client, 0, sizeof client); memset(&routed, 0, sizeof routed);
    cn_reader_position_clear(&a); cn_reader_position_clear(&b);
    cn_reader_position_clear(&snapshot); cn_progress_record_clear(&persisted);
    free(exact_path); cn_ui_free(ui); cn_library_free(library);
    cn_progress_store_close(store);
    if (rc) puts("SYNC CONFLICT GATE result=failed");
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--smoke"))
        return smoke(argv[2], argv[3], argv[4]);
    if (argc > 1 && !strcmp(argv[1], "--physical"))
        return physical(argc, argv);
    fputs("usage: sync-conflict-ui-test --smoke <font> <epub> <existing-state-dir> | --physical <seed-remote|run|verify-remote> <reserved> <mount> <root> <major> <minor> <font> <epub> <dns-ip> <dns-port> <sntp-ip> <sntp-port> <ca>\n", stderr);
    return 2;
}
