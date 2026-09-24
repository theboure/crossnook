/* Save-first Reader sync smoke and later verified-card diagnostic. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/reader_sync.h"
#include "graphics/canvas.h"
#include "platform/storage_verify.h"
#include "storage/storage_layout.h"

/* Existing controlled mock account fixture; never print or pass through argv. */
#define MOCK_KEY "dfb450efddbb5387197c84460623675b"
#define MOCK_POSITION_TWO "/body/DocFragment[1]/body/p[3]/text().5"

static const char *scenario_account(const char *scenario)
{
    if (!strcmp(scenario, "upload")) return "integration-local-only";
    if (!strcmp(scenario, "no-change")) return "integration-local-only";
    if (!strcmp(scenario, "conflict")) return "integration-different";
    return NULL;
}

typedef enum fake_action {
    FAKE_OUTCOME, FAKE_IMPORT_B, FAKE_IMPORT_MISSING,
    FAKE_IMPORT_CORRUPT, FAKE_IMPORT_UNSUPPORTED, FAKE_IMPORT_BAD_POINTER
} fake_action;
static int fake_controller, fail_identity, fail_capture, fail_save;
static int controller_calls, save_calls, capture_calls, identity_calls;
static int failures;
static fake_action action;
static cn_kosync_product_outcome fake_outcome;
static cn_progress_store *fake_store;
static const char *fake_directory;
static const char *expected_position;
static cn_reader_position position_b;

int __real_cn_book_identity_from_path(cn_book_identity *, const char *);
int __real_cn_ui_reader_get_position(cn_ui *, cn_reader_position *);
cn_progress_result __real_cn_progress_store_save(cn_progress_store *,
                                                  const cn_book_identity *,
                                                  const cn_progress_record *);
cn_sync_controller_result __real_cn_sync_current_book(
    const cn_sync_controller_config *);

int __wrap_cn_book_identity_from_path(cn_book_identity *id, const char *path)
{
    if (fake_controller) {
        ++identity_calls;
        if (fail_identity) return -1;
    }
    return __real_cn_book_identity_from_path(id, path);
}
int __wrap_cn_ui_reader_get_position(cn_ui *ui, cn_reader_position *position)
{
    if (fake_controller) {
        ++capture_calls;
        if (fail_capture) return -1;
    }
    return __real_cn_ui_reader_get_position(ui, position);
}
cn_progress_result __wrap_cn_progress_store_save(
    cn_progress_store *store, const cn_book_identity *id,
    const cn_progress_record *record)
{
    if (fake_controller) {
        ++save_calls;
        if (fail_save) return CN_PROGRESS_IO_ERROR;
    }
    return __real_cn_progress_store_save(store, id, record);
}

cn_sync_controller_result __wrap_cn_sync_current_book(
    const cn_sync_controller_config *config)
{
    cn_sync_controller_result result;
    cn_book_identity id;
    cn_progress_record loaded;
    cn_progress_record remote;
    char path[CN_STORAGE_PATH_CAPACITY];
    int fd;
    if (!fake_controller) return __real_cn_sync_current_book(config);
    ++controller_calls;
    if (!config || config->progress_store != fake_store ||
        !config->document_path || !expected_position ||
        __real_cn_book_identity_from_path(&id, config->document_path) != 0) {
        ++failures;
    } else {
        cn_progress_record_init(&loaded);
        if (cn_progress_store_load(fake_store, &id, &loaded) != CN_PROGRESS_OK ||
            !loaded.position.location ||
            strcmp(loaded.position.location, expected_position) != 0)
            ++failures;
        cn_progress_record_clear(&loaded);
    }
    memset(&result, 0, sizeof result);
    result.stage = CN_SYNC_CONTROLLER_INTEGRATION;
    result.product.outcome = fake_outcome;
    result.product.retry = fake_outcome == CN_KOSYNC_PRODUCT_CONFLICT
                               ? CN_KOSYNC_RETRY_EXPLICIT_ACTION : CN_KOSYNC_RETRY_NONE;
    if (action == FAKE_OUTCOME) return result;
    result.product.outcome = CN_KOSYNC_PRODUCT_IMPORTED;
    result.product.local_mutation = CN_KOSYNC_MUTATION_CONFIRMED;
    if (action == FAKE_IMPORT_MISSING || action == FAKE_IMPORT_CORRUPT ||
        action == FAKE_IMPORT_UNSUPPORTED) {
        const char *token = cn_book_identity_token(&id);
        if (!token || snprintf(path, sizeof path, "%s/%s.progress",
                               fake_directory, token) >= (int)sizeof path) {
            ++failures;
            return result;
        }
        if (action == FAKE_IMPORT_MISSING) {
            if (unlink(path) != 0) ++failures;
        } else if (action == FAKE_IMPORT_UNSUPPORTED) {
            fd = open(path, O_WRONLY);
            if (fd < 0) ++failures;
            else {
                if (lseek(fd, 11, SEEK_SET) != 11 || write(fd, "\2", 1) != 1)
                    ++failures;
                if (close(fd) != 0) ++failures;
            }
        } else {
            fd = open(path, O_WRONLY | O_TRUNC);
            if (fd < 0 || write(fd, "bad", 3) != 3 || close(fd) != 0)
                ++failures;
        }
        return result;
    }
    cn_progress_record_init(&remote);
    if (action == FAKE_IMPORT_BAD_POINTER) {
        remote.position.location = (char *)"/body/DocFragment[999]/body/p[999]/text().0";
        remote.position.progress_10000 = 1234;
    } else remote.position = position_b; /* borrowed only for this synchronous save */
    if (__real_cn_progress_store_save(fake_store, &id, &remote) != CN_PROGRESS_OK)
        ++failures;
    remote.position.location = NULL;
    return result;
}

static void check(int condition, const char *label)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", label);
    fflush(stdout);
    if (!condition) ++failures;
}
static void reset_fake(cn_kosync_product_outcome outcome)
{
    fake_controller = 1;
    fail_identity = fail_capture = fail_save = 0;
    controller_calls = save_calls = capture_calls = identity_calls = 0;
    action = FAKE_OUTCOME; fake_outcome = outcome;
}
static void touch(cn_ui *ui, int y)
{
    cn_input_ev ev;
    memset(&ev, 0, sizeof ev);
    ev.type = CN_INPUT_TOUCH_UP; ev.x = 200; ev.y = y;
    (void)cn_ui_handle(ui, &ev);
}
static void turn(cn_ui *ui, int count)
{
    cn_input_ev ev;
    memset(&ev, 0, sizeof ev); ev.type = CN_INPUT_PAGE_NEXT;
    while (count-- > 0) (void)cn_ui_handle(ui, &ev);
}
static cn_ui *open_ui(const char *font, const char *book, cn_library **out_library)
{
    cn_ui *ui = cn_ui_init();
    cn_library *lib = cn_library_new();
    cn_reader_config cfg;
    memset(&cfg, 0, sizeof cfg); cfg.font_path = font;
    if (!ui || !lib || cn_library_add(lib, book, "synthetic.epub", CN_BOOK_EPUB) < 0 ||
        cn_ui_set_reader(ui, &cfg) != 0 || cn_ui_set_library(ui, lib) != 1) {
        cn_ui_free(ui); cn_library_free(lib); return NULL;
    }
    *out_library = lib;
    touch(ui, 320); /* HOME -> LIBRARY */
    touch(ui, CN_UI_LIB_ROW_TOP + 10); /* first selected EPUB -> READER */
    if (cn_ui_get_state(ui) != CN_UI_READER) {
        cn_ui_free(ui); cn_library_free(lib); return NULL;
    }
    return ui;
}
static int capture_equal(cn_ui *ui, const cn_reader_position *expected)
{
    cn_reader_position got;
    int equal;
    cn_reader_position_init(&got);
    equal = cn_ui_reader_get_position(ui, &got) == 0 && got.location &&
            expected->location && strcmp(got.location, expected->location) == 0;
    cn_reader_position_clear(&got);
    return equal;
}

static int smoke(const char *font, const char *book, const char *directory)
{
    cn_ui *ui;
    cn_library *library = NULL;
    cn_progress_store *store = NULL;
    cn_sync_controller_config controller;
    cn_reader_sync_config config;
    cn_reader_sync_result result;
    cn_reader_position position_a;
    cn_progress_record loaded;
    int i;
    const cn_kosync_product_outcome neutral[] = {
        CN_KOSYNC_PRODUCT_UPLOADED, CN_KOSYNC_PRODUCT_UNCHANGED,
        CN_KOSYNC_PRODUCT_NO_STATE, CN_KOSYNC_PRODUCT_CONFLICT,
        CN_KOSYNC_PRODUCT_AUTH_REQUIRED, CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE
    };
    cn_reader_position_init(&position_a);
    cn_reader_position_init(&position_b);
    cn_progress_record_init(&loaded);
    if (cn_progress_store_open(&store, directory) != CN_PROGRESS_OK) return 1;
    ui = cn_ui_init();
    memset(&controller, 0, sizeof controller);
    config.ui = ui; config.progress_store = store; config.controller = &controller;
    reset_fake(CN_KOSYNC_PRODUCT_UPLOADED);
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_NOT_OPEN && !controller_calls && !save_calls,
          "closed Reader cannot sync");
    cn_ui_free(ui);
    ui = open_ui(font, book, &library);
    if (!ui) { cn_progress_store_close(store); return 1; }
    config.ui = ui;
    cn_ui_set_library(ui, NULL);
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_NOT_OPEN && !controller_calls,
          "Reader with no selected book is rejected");
    cn_ui_set_library(ui, library);
    controller.document_path = "/tmp/different-book.epub";
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_CONFIG_MISMATCH && !controller_calls && !save_calls,
          "conflicting controller document path rejected");
    controller.document_path = NULL;
    controller.progress_store = (cn_progress_store *)&controller;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_CONFIG_MISMATCH && !controller_calls,
          "conflicting progress store rejected");
    controller.progress_store = NULL;
    fail_identity = 1;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_IDENTITY_FAILED && !controller_calls && !save_calls,
          "identity failure stops before save/sync");
    fail_identity = 0; fail_capture = 1;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_CAPTURE_FAILED && !controller_calls && !save_calls,
          "capture failure stops before save/sync");
    fail_capture = 0;
    turn(ui, 5);
    check(cn_ui_reader_get_position(ui, &position_a) == 0 &&
          position_a.location && position_a.progress_10000 >= 0,
          "Reader A captured without closing document");
    check(position_a.location && strcmp(position_a.location, MOCK_POSITION_TWO) != 0,
          "Reader A differs from controlled conflict fixture");
    expected_position = position_a.location;
    fake_store = store; fake_directory = directory;
    fail_save = 1;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_PRE_SAVE_FAILED &&
          result.pre_save_result == CN_PROGRESS_IO_ERROR &&
          result.pre_save_attempted && !result.controller_invoked &&
          !controller_calls && save_calls == 1,
          "failed pre-save stops before controller");
    fail_save = 0;
    for (i = 0; i < (int)(sizeof neutral / sizeof neutral[0]); ++i) {
        cn_book_identity id;
        reset_fake(neutral[i]);
        result = cn_reader_sync_manual_once(&config);
        check(result.phase == CN_READER_SYNC_CONTROLLER_COMPLETE &&
              result.controller.product.outcome == neutral[i] &&
              result.pre_save_result == CN_PROGRESS_OK &&
              result.pre_save_attempted && result.controller_invoked &&
              controller_calls == 1 && save_calls == 1 &&
              identity_calls == 1 && capture_calls == 1 &&
              !result.redraw_needed && !result.movement_possible &&
              capture_equal(ui, &position_a),
              "one save, one controller, no Reader move for non-import");
        check(__real_cn_book_identity_from_path(&id, book) == 0 &&
              cn_progress_store_load(store, &id, &loaded) == CN_PROGRESS_OK &&
              loaded.position.location &&
              strcmp(loaded.position.location, position_a.location) == 0,
              "pre-sync ProgressStore record equals captured A");
    }
    /* Capture B from a different real Reader page; restore A before the call. */
    {
        cn_input_ev ev;
        memset(&ev, 0, sizeof ev); ev.type = CN_INPUT_PAGE_PREV;
        for (i = 0; i < 4; ++i) (void)cn_ui_handle(ui, &ev);
    }
    check(cn_ui_reader_get_position(ui, &position_b) == 0 &&
          position_b.location && position_a.location &&
          strcmp(position_b.location, position_a.location) != 0 &&
          cn_ui_reader_goto_position(ui, &position_a) == 0,
          "distinct real Reader position B captured");
    reset_fake(CN_KOSYNC_PRODUCT_IMPORTED); action = FAKE_IMPORT_B;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_IMPORT_APPLIED &&
          result.pre_save_result == CN_PROGRESS_OK &&
          result.import_reload_result == CN_PROGRESS_OK &&
          result.controller.product.outcome == CN_KOSYNC_PRODUCT_IMPORTED &&
          result.controller_invoked && controller_calls == 1 && save_calls == 1 &&
          result.movement_possible && result.redraw_needed &&
          capture_equal(ui, &position_b),
          "defensive injected import reloads authoritative B and applies Reader");
    check(cn_ui_reader_goto_position(ui, &position_a) == 0,
          "Reader reset to A for failed-import cases");
    reset_fake(CN_KOSYNC_PRODUCT_IMPORTED); action = FAKE_IMPORT_MISSING;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_IMPORT_RELOAD_FAILED &&
          result.import_reload_result == CN_PROGRESS_MISSING &&
          result.disk_reader_mismatch_possible && !result.redraw_needed &&
          capture_equal(ui, &position_a), "missing imported record leaves Reader at A");
    reset_fake(CN_KOSYNC_PRODUCT_IMPORTED); action = FAKE_IMPORT_CORRUPT;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_IMPORT_RELOAD_FAILED &&
          result.import_reload_result == CN_PROGRESS_CORRUPT &&
          capture_equal(ui, &position_a), "corrupt imported record not applied");
    reset_fake(CN_KOSYNC_PRODUCT_IMPORTED); action = FAKE_IMPORT_UNSUPPORTED;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_IMPORT_RELOAD_FAILED &&
          result.import_reload_result == CN_PROGRESS_UNSUPPORTED &&
          capture_equal(ui, &position_a), "unsupported imported record not applied");
    reset_fake(CN_KOSYNC_PRODUCT_IMPORTED); action = FAKE_IMPORT_BAD_POINTER;
    result = cn_reader_sync_manual_once(&config);
    check(result.phase == CN_READER_SYNC_IMPORT_APPLY_FAILED &&
          result.import_reload_result == CN_PROGRESS_OK &&
          result.disk_reader_mismatch_possible && !result.redraw_needed &&
          capture_equal(ui, &position_a), "unresolvable imported XPointer never moves Reader");
    /* Separate apply helper: operates only on previously persisted B. */
    {
        cn_progress_record remote;
        cn_book_identity id;
        cn_progress_record_init(&remote);
        remote.position = position_b;
        if (__real_cn_book_identity_from_path(&id, book) != 0 ||
            __real_cn_progress_store_save(store, &id, &remote) != CN_PROGRESS_OK)
            ++failures;
        remote.position.location = NULL;
        reset_fake(CN_KOSYNC_PRODUCT_IMPORTED);
        result = cn_reader_sync_apply_persisted(ui, store);
        check(result.phase == CN_READER_SYNC_IMPORT_APPLIED &&
              result.import_reload_result == CN_PROGRESS_OK &&
              !result.controller_invoked && !result.pre_save_attempted &&
              result.redraw_needed && capture_equal(ui, &position_b),
              "apply-persisted is a no-network/no-save Reader refresh");
    }
    fake_controller = 0;
    cn_progress_record_clear(&loaded);
    cn_reader_position_clear(&position_b);
    cn_reader_position_clear(&position_a);
    cn_ui_free(ui);
    cn_library_free(library);
    cn_progress_store_close(store);
    printf("READER SYNC SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int parse_unsigned(const char *text, unsigned long limit, unsigned long *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !text[0] || text[0] == '-') return 0;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed > limit) return 0;
    *value = parsed;
    return 1;
}

static int on_verified_card(const char *path, const struct stat *root)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_dev == root->st_dev;
}

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
    cn_sync_controller_config controller;
    cn_reader_sync_config config;
    cn_reader_sync_result result;
    cn_dns_config dns;
    cn_time_config time;
    cn_tls_config tls;
    cn_reader_position position_a, position_b;
    cn_progress_record saved;
    cn_book_identity identity;
    cn_ui *ui = NULL;
    cn_library *lib = NULL;
    cn_canvas *canvas = NULL;
    struct stat root_st;
    char gate[CN_STORAGE_PATH_CAPACITY], progress[CN_STORAGE_PATH_CAPACITY];
    char config_dir[CN_STORAGE_PATH_CAPACITY], state[CN_STORAGE_PATH_CAPACITY];
    unsigned long major_number, minor_number, dns_port, time_port;
    const char *scenario, *account;
    int rc = 1, unchanged, persisted_a = 0, rendered = 0;
    int saved_stderr = -1, sink = -1;

    if (argc != 15 || strcmp(argv[2], "run") != 0) return 2;
    scenario = argv[3]; account = scenario_account(scenario);
    if (!account && strcmp(scenario, "apply-demo")) return 2;
    if (!parse_unsigned(argv[6], 0xffffffffUL, &major_number) ||
        !parse_unsigned(argv[7], 0xffffffffUL, &minor_number) ||
        !parse_unsigned(argv[11], 65535, &dns_port) || !dns_port ||
        !parse_unsigned(argv[13], 65535, &time_port) || !time_port) return 2;
    candidate.mountpoint = argv[4]; candidate.root = argv[5];
    candidate.expected_major = (unsigned)major_number;
    candidate.expected_minor = (unsigned)minor_number;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) != CN_PLATFORM_STORAGE_OK) {
        puts("READER SYNC GATE storage=unverified persistence=not-attempted");
        return 1;
    }
    if (stat(verified.root, &root_st) != 0 ||
         snprintf(gate, sizeof gate, "%s/%s%s", verified.root,
                  account ? "controller-" : "reader-sync-",
                  !strcmp(scenario, "no-change") ? "upload" : scenario) >=
            (int)sizeof gate) return 1;
    if (!account && mkdir(gate, 0700) != 0) return 1;
    if (!on_verified_card(gate, &root_st) ||
        cn_storage_layout_init(&layout, gate, NULL) != CN_STORAGE_OK ||
        (!account && cn_storage_layout_prepare(&layout, NULL) != CN_STORAGE_OK) ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG,
                               config_dir, sizeof config_dir) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                               progress, sizeof progress) != CN_STORAGE_OK ||
        snprintf(state, sizeof state, "%s/state", gate) >= (int)sizeof state ||
        !on_verified_card(state, &root_st) ||
        !on_verified_card(progress, &root_st) ||
        !on_verified_card(config_dir, &root_st) ||
        cn_progress_store_open(&store, progress) != CN_PROGRESS_OK) return 1;

    cn_reader_position_init(&position_a);
    cn_reader_position_init(&position_b);
    cn_progress_record_init(&saved);
    memset(&credentials, 0, sizeof credentials);
    /* The published Reader logs its book path on open. The device diagnostic
     * exposes only bounded status; suppress underlying Reader stderr here. */
    saved_stderr = dup(STDERR_FILENO);
    sink = open("/dev/null", O_WRONLY);
    if (saved_stderr < 0 || sink < 0 || dup2(sink, STDERR_FILENO) < 0)
        goto done;
    close(sink); sink = -1;
    ui = open_ui(argv[8], argv[9], &lib);
    if (!ui) goto done;
    turn(ui, 5);
    if (cn_ui_reader_get_position(ui, &position_a) != 0 ||
        !position_a.location) goto done;
    if (account && !strcmp(scenario, "conflict") &&
        strcmp(position_a.location, MOCK_POSITION_TWO) == 0) {
        /* Do not claim conflict if A accidentally equals remote mock B. */
        goto done;
    }

    if (!account) {
        cn_input_ev ev;
        int i;
        memset(&ev, 0, sizeof ev); ev.type = CN_INPUT_PAGE_PREV;
        for (i = 0; i < 4; ++i) (void)cn_ui_handle(ui, &ev);
        if (cn_ui_reader_get_position(ui, &position_b) != 0 ||
            !position_b.location ||
            strcmp(position_a.location, position_b.location) == 0 ||
            __real_cn_book_identity_from_path(&identity, argv[9]) != 0)
            goto done;
        saved.position = position_b;
        if (__real_cn_progress_store_save(store, &identity, &saved) != CN_PROGRESS_OK)
            goto done;
        saved.position.location = NULL;
        if (cn_ui_reader_goto_position(ui, &position_a) != 0) goto done;
        result = cn_reader_sync_apply_persisted(ui, store);
        unchanged = capture_equal(ui, &position_b);
        canvas = cn_canvas_create(CN_READER_W, CN_READER_H);
        if (canvas && result.redraw_needed) {
            const uint16_t *pixels;
            size_t i;
            cn_ui_render(ui, canvas, NULL); /* memory only; caller owns display flush */
            pixels = (const uint16_t *)cn_canvas_pixels(canvas);
            if (pixels)
                for (i = 0; i < (size_t)CN_READER_W * CN_READER_H; ++i)
                    if (pixels[i] != CN_COLOR_WHITE) {
                        rendered = 1;
                        break;
                    }
        }
        printf("READER SYNC GATE phase=%s reload=%s controller-invoked=%d "
               "redraw-needed=%d movement-possible=%d recapture-matched=%s "
               "render=%s\n", cn_reader_sync_phase_name(result.phase),
               cn_progress_result_name(result.import_reload_result),
               result.controller_invoked, result.redraw_needed,
               result.movement_possible, unchanged ? "yes" : "no",
               rendered ? "ok" : "failed");
        rc = result.phase == CN_READER_SYNC_IMPORT_APPLIED &&
             unchanged && rendered ? 0 : 1;
        goto done;
    }
    if (cn_settings_store_init(&settings_store, config_dir, NULL) != CN_SETTINGS_OK ||
        cn_credential_store_init(&credential_store, state, NULL) != CN_CREDENTIAL_OK ||
        cn_settings_load(&settings_store, &settings, NULL) != CN_SETTINGS_OK ||
        !settings.kosync_enabled ||
        cn_credential_store_load(&credential_store, &credentials, NULL) != CN_CREDENTIAL_OK ||
        strcmp(credentials.username, account) ||
        strcmp(credentials.userkey, MOCK_KEY)) goto done;
    cn_credentials_clear(&credentials);
    memset(&dns, 0, sizeof dns);
    dns.servers[0] = argv[10]; dns.server_count = 1; dns.port = (unsigned)dns_port;
    memset(&time, 0, sizeof time);
    time.servers[0] = argv[12]; time.server_count = 1; time.port = (unsigned)time_port;
    memset(&tls, 0, sizeof tls); tls.ca_path = argv[14];
    memset(&controller, 0, sizeof controller);
    controller.settings_store = &settings_store;
    controller.credential_store = &credential_store;
    controller.dns = &dns; controller.time = &time; controller.tls = &tls;
    controller.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    controller.device_id = "synthetic-reader-sync-device-id";
    config.ui = ui; config.progress_store = store; config.controller = &controller;
    result = cn_reader_sync_manual_once(&config);
    unchanged = capture_equal(ui, &position_a);
    if (__real_cn_book_identity_from_path(&identity, argv[9]) == 0 &&
        cn_progress_store_load(store, &identity, &saved) == CN_PROGRESS_OK &&
        saved.position.location && position_a.location &&
        strcmp(saved.position.location, position_a.location) == 0)
        persisted_a = 1;
    printf("READER SYNC GATE phase=%s pre-save=%s controller-invoked=%d "
           "outcome=%s retry=%s local-mutation=%s remote-mutation=%s "
           "redraw-needed=%d movement-possible=%d reader-unchanged=%s "
           "persisted-a=%s\n",
           cn_reader_sync_phase_name(result.phase),
           cn_progress_result_name(result.pre_save_result),
           result.controller_invoked,
           cn_kosync_product_outcome_name(result.controller.product.outcome),
           cn_kosync_retry_policy_name(result.controller.product.retry),
           cn_kosync_mutation_state_name(result.controller.product.local_mutation),
           cn_kosync_mutation_state_name(result.controller.product.remote_mutation),
           result.redraw_needed, result.movement_possible,
           unchanged ? "yes" : "no", persisted_a ? "yes" : "no");
    rc = result.phase == CN_READER_SYNC_CONTROLLER_COMPLETE &&
         result.pre_save_result == CN_PROGRESS_OK &&
         result.controller_invoked && unchanged && persisted_a &&
         result.controller.stage == CN_SYNC_CONTROLLER_INTEGRATION &&
         ((!strcmp(scenario, "upload") &&
           result.controller.product.outcome == CN_KOSYNC_PRODUCT_UPLOADED &&
           result.controller.remote_put_attempted == 1) ||
          (!strcmp(scenario, "no-change") &&
           result.controller.product.outcome == CN_KOSYNC_PRODUCT_UNCHANGED &&
           result.controller.remote_put_attempted == 0) ||
          (!strcmp(scenario, "conflict") &&
           result.controller.product.outcome == CN_KOSYNC_PRODUCT_CONFLICT &&
           result.controller.remote_put_attempted == 0)) ? 0 : 1;
 done:
    if (saved_stderr >= 0) {
        fflush(stderr);
        (void)dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    if (sink >= 0) close(sink);
    cn_credentials_clear(&credentials);
    cn_reader_position_clear(&position_a);
    cn_reader_position_clear(&position_b);
    cn_progress_record_clear(&saved);
    cn_canvas_free(canvas);
    cn_ui_free(ui);
    cn_library_free(lib);
    cn_progress_store_close(store);
    if (rc != 0) puts("READER SYNC GATE result=failed");
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--smoke") == 0)
        return smoke(argv[2], argv[3], argv[4]);
    if (argc >= 2 && strcmp(argv[1], "--physical") == 0)
        return physical(argc, argv);
    fputs("usage: reader-sync-test --smoke <font> <epub> <existing-directory> | --physical run <upload|no-change|conflict|apply-demo> <mount> <root> <major> <minor> <font> <epub> <dns-ip> <dns-port> <sntp-ip> <sntp-port> <ca>\n", stderr);
    return 2;
}
