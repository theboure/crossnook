/* Synthetic GET-only pull matrix and later verified-card diagnostic. */
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
#include "platform/storage_verify.h"
#include "storage/storage_layout.h"

#define DUMMY_USER "crossnook-synthetic-pull-account"
#define DUMMY_KEY "crossnook-synthetic-pull-key"
#define MOCK_KEY "dfb450efddbb5387197c84460623675b"
#define REMOTE_FIXTURE "/body/DocFragment[1]/body/p[3]/text().5"
#define LOCAL_FIXTURE "/body/DocFragment[1]/body/p[1]/text().0"

static int fake;
static int enabled, invalid_url, save_fault;
static cn_settings_result settings_mode;
static cn_credential_result credential_mode;
static cn_time_result time_mode;
static cn_dns_result dns_mode;
static cn_kosync_result get_mode;
static cn_netsimple_result transport_mode;
static const char *remote_location;
static int settings_calls, credential_calls, time_calls, dns_calls, get_calls;
static int put_calls, normal_sync_calls, save_calls, credential_clear_calls;
static int failures;

static void clear_bytes(void *data, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (size--) *p++ = 0;
}

cn_settings_result __real_cn_settings_load(const cn_settings_store *, cn_settings *, int *);
cn_credential_result __real_cn_credential_store_load(const cn_credential_store *, cn_credentials *, int *);
void __real_cn_credentials_clear(cn_credentials *);
cn_time_result __real_cn_timesimple_sync(const cn_time_config *, cn_time_sample *);
cn_dns_result __real_cn_dnssimple_resolve_a(const cn_dns_config *, const char *, cn_dns_answer *);
cn_kosync_result __real_cn_kosync_get_progress(const cn_kosync_client *, const char *, cn_kosync_progress *, cn_kosync_outcome *);
cn_kosync_result __real_cn_kosync_put_progress(const cn_kosync_client *, const cn_kosync_progress *, long long *, cn_kosync_outcome *);
cn_kosync_sync_status __real_cn_kosync_sync_once(const cn_kosync_sync_config *, cn_kosync_sync_result *);
cn_progress_result __real_cn_progress_store_save(cn_progress_store *, const cn_book_identity *, const cn_progress_record *);

cn_settings_result __wrap_cn_settings_load(const cn_settings_store *store,
                                           cn_settings *out, int *error)
{
    if (!fake) return __real_cn_settings_load(store, out, error);
    ++settings_calls;
    cn_settings_defaults(out);
    if (settings_mode == CN_SETTINGS_OK) {
        out->kosync_enabled = enabled;
        if (!invalid_url)
            strcpy(out->kosync_base_url, "https://sync.synthetic.invalid/");
        else strcpy(out->kosync_base_url, "http://sync.synthetic.invalid/");
    }
    return settings_mode;
}
cn_credential_result __wrap_cn_credential_store_load(
    const cn_credential_store *store, cn_credentials *out, int *error)
{
    if (!fake) return __real_cn_credential_store_load(store, out, error);
    (void)store; (void)error;
    ++credential_calls;
    strcpy(out->username, DUMMY_USER);
    strcpy(out->userkey, DUMMY_KEY);
    return credential_mode;
}
void __wrap_cn_credentials_clear(cn_credentials *value)
{
    if (fake) ++credential_clear_calls;
    __real_cn_credentials_clear(value);
}
cn_time_result __wrap_cn_timesimple_sync(const cn_time_config *config,
                                         cn_time_sample *sample)
{
    if (!fake) return __real_cn_timesimple_sync(config, sample);
    (void)config; (void)sample;
    ++time_calls;
    return time_mode;
}
cn_dns_result __wrap_cn_dnssimple_resolve_a(
    const cn_dns_config *config, const char *hostname, cn_dns_answer *answer)
{
    if (!fake) return __real_cn_dnssimple_resolve_a(config, hostname, answer);
    (void)config;
    ++dns_calls;
    if (!hostname || strcmp(hostname, "sync.synthetic.invalid")) ++failures;
    memset(answer, 0, sizeof *answer);
    if (dns_mode == CN_DNS_OK) {
        answer->count = 1;
        strcpy(answer->ipv4[0], "127.0.0.2");
    }
    return dns_mode;
}
cn_kosync_result __wrap_cn_kosync_get_progress(
    const cn_kosync_client *client, const char *document,
    cn_kosync_progress *progress, cn_kosync_outcome *outcome)
{
    if (!fake) return __real_cn_kosync_get_progress(client, document, progress,
                                                    outcome);
    ++get_calls;
    if (!client || !client->use_tls || !client->tls ||
        strcmp(client->host, "sync.synthetic.invalid") ||
        strcmp(client->connect_host, "127.0.0.2") ||
        strcmp(client->username, DUMMY_USER) ||
        strcmp(client->userkey, DUMMY_KEY)) ++failures;
    if (outcome) {
        outcome->http_status = get_mode == CN_KOSYNC_AUTH_FAILED ? 401 :
                               get_mode == CN_KOSYNC_HTTP_ERROR ? 404 : 200;
        outcome->transport_result = get_mode == CN_KOSYNC_TRANSPORT_ERROR
                                        ? transport_mode : CN_NETSIMPLE_OK;
    }
    if (get_mode != CN_KOSYNC_OK) return get_mode;
    return cn_kosync_progress_set(progress, document, remote_location,
                                  6543, "remote-synthetic-device",
                                  "remote-synthetic-id");
}
cn_kosync_result __wrap_cn_kosync_put_progress(
    const cn_kosync_client *client, const cn_kosync_progress *progress,
    long long *timestamp, cn_kosync_outcome *outcome)
{
    ++put_calls;
    if (fake) { ++failures; return CN_KOSYNC_INVALID; }
    return __real_cn_kosync_put_progress(client, progress, timestamp, outcome);
}
cn_kosync_sync_status __wrap_cn_kosync_sync_once(
    const cn_kosync_sync_config *config, cn_kosync_sync_result *result)
{
    ++normal_sync_calls;
    if (fake) { ++failures; return CN_KOSYNC_SYNC_STATUS_INVALID; }
    return __real_cn_kosync_sync_once(config, result);
}
cn_progress_result __wrap_cn_progress_store_save(
    cn_progress_store *store, const cn_book_identity *identity,
    const cn_progress_record *record)
{
    if (fake) {
        ++save_calls;
        if (save_fault) return CN_PROGRESS_IO_ERROR;
    }
    return __real_cn_progress_store_save(store, identity, record);
}

static void check(int condition, const char *name)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    fflush(stdout);
    if (!condition) ++failures;
}
static void reset_fake(void)
{
    fake = enabled = 1;
    invalid_url = save_fault = 0;
    settings_mode = CN_SETTINGS_OK;
    credential_mode = CN_CREDENTIAL_OK;
    time_mode = CN_TIME_OK; dns_mode = CN_DNS_OK;
    get_mode = CN_KOSYNC_OK; transport_mode = CN_NETSIMPLE_OK;
    remote_location = REMOTE_FIXTURE;
    settings_calls = credential_calls = time_calls = dns_calls = get_calls = 0;
    put_calls = normal_sync_calls = save_calls = credential_clear_calls = 0;
}
static int record_equals(cn_progress_store *store, const char *book,
                         const char *position)
{
    cn_book_identity id;
    cn_progress_record record;
    int ok;
    if (cn_book_identity_from_path(&id, book) != 0) return 0;
    cn_progress_record_init(&record);
    ok = cn_progress_store_load(store, &id, &record) == CN_PROGRESS_OK &&
         record.position.location && !strcmp(record.position.location, position);
    cn_progress_record_clear(&record);
    return ok;
}
static int seed(cn_progress_store *store, const char *book, const char *position)
{
    cn_book_identity id;
    cn_progress_record record;
    cn_progress_result result;
    if (cn_book_identity_from_path(&id, book) != 0) return 0;
    cn_progress_record_init(&record);
    record.position.location = (char *)position;
    record.position.progress_10000 = 3210;
    result = __real_cn_progress_store_save(store, &id, &record);
    record.position.location = NULL;
    return result == CN_PROGRESS_OK;
}
static cn_sync_pull_result invoke(cn_progress_store *store, const char *book)
{
    cn_sync_controller_config config;
    cn_dns_config dns = {{"127.0.0.2"}, 1, 53, 200};
    cn_time_config time = {{"127.0.0.2"}, 1, 123, 200};
    cn_tls_config tls = {"/tmp/synthetic-ca", NULL, NULL};
    memset(&config, 0, sizeof config);
    config.settings_store = (const cn_settings_store *)&config;
    config.credential_store = (const cn_credential_store *)&config;
    config.progress_store = store;
    config.document_path = book;
    config.device_id = NULL; /* GET-only path must not require one. */
    config.dns = &dns; config.time = &time; config.tls = &tls;
    config.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    return cn_sync_pull_remote_current_book(&config);
}

static int smoke(const char *font, const char *book, const char *directory)
{
    cn_progress_store *store = NULL;
    cn_sync_pull_result result;
    cn_reader_position position_b;
    cn_ui *ui = NULL;
    cn_library *library = NULL;
    cn_reader_sync_result applied;
    cn_input_ev event;
    int rc;
    cn_reader_position_init(&position_b);
    if (cn_progress_store_open(&store, directory) != CN_PROGRESS_OK) return 1;
    reset_fake(); settings_mode = CN_SETTINGS_MISSING;
    result = invoke(store, book);
    check(result.stage == CN_SYNC_PULL_DISABLED &&
          result.outcome == CN_SYNC_PULL_DISABLED_OUTCOME &&
          !credential_calls && !time_calls && !dns_calls && !get_calls &&
          !save_calls && !put_calls && !normal_sync_calls,
          "missing Settings disabled before credentials and network");
    reset_fake(); enabled = 0;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_DISABLED_OUTCOME &&
          !credential_calls && !get_calls, "explicit disabled short-circuits");
    reset_fake(); settings_mode = CN_SETTINGS_CORRUPT;
    result = invoke(store, book);
    check(result.stage == CN_SYNC_PULL_SETTINGS_FAILED &&
          result.settings_result == CN_SETTINGS_CORRUPT &&
          result.outcome == CN_SYNC_PULL_CONFIGURATION_FAILURE &&
          !credential_calls && !get_calls,
          "corrupt Settings are not disabled defaults");
    reset_fake(); credential_mode = CN_CREDENTIAL_MISSING;
    result = invoke(store, book);
    check(result.stage == CN_SYNC_PULL_CREDENTIALS_FAILED &&
          result.outcome == CN_SYNC_PULL_AUTH_REQUIRED &&
          result.credential_result == CN_CREDENTIAL_MISSING &&
          credential_clear_calls == 1 && !get_calls,
          "missing credentials require explicit authentication");
    reset_fake(); credential_mode = CN_CREDENTIAL_CORRUPT;
    result = invoke(store, book);
    check(result.credential_result == CN_CREDENTIAL_CORRUPT &&
          result.outcome == CN_SYNC_PULL_CONFIGURATION_FAILURE && !get_calls,
          "corrupt credentials distinct from missing");
    reset_fake(); credential_mode = CN_CREDENTIAL_UNSUPPORTED_VERSION;
    result = invoke(store, book);
    check(result.credential_result == CN_CREDENTIAL_UNSUPPORTED_VERSION &&
          !get_calls, "unsupported credentials stop preflight");
    reset_fake(); credential_mode = CN_CREDENTIAL_IO_ERROR;
    result = invoke(store, book);
    check(result.stage == CN_SYNC_PULL_CREDENTIALS_FAILED &&
          result.outcome == CN_SYNC_PULL_LOCAL_FAILURE && !get_calls,
          "credential I/O failure is not missing authentication");
    reset_fake(); invalid_url = 1;
    result = invoke(store, book);
    check(result.stage == CN_SYNC_PULL_CONFIG_FAILED && !get_calls,
          "plaintext URL refused before network");
    reset_fake(); get_mode = CN_KOSYNC_NOT_FOUND;
    check(seed(store, book, LOCAL_FIXTURE), "existing local A fixture");
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_REMOTE_MISSING_OUTCOME &&
          result.pull.get_attempted == 1 && !result.pull.local_save_attempted &&
          result.local_mutation == CN_KOSYNC_MUTATION_NONE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_NONE &&
          record_equals(store, book, LOCAL_FIXTURE) && get_calls == 1 &&
          save_calls == 0 && put_calls == 0 && normal_sync_calls == 0,
          "fresh remote missing preserves local A, GET=1 PUT=0");
    reset_fake();
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
          result.pull.get_attempted && result.pull.local_save_attempted &&
          result.pull.local_saved &&
          result.local_mutation == CN_KOSYNC_MUTATION_CONFIRMED &&
          result.remote_mutation == CN_KOSYNC_MUTATION_NONE &&
          get_calls == 1 && save_calls == 1 && !put_calls &&
          record_equals(store, book, REMOTE_FIXTURE),
          "authorized remote B overwrites differing local A without PUT");
    reset_fake(); remote_location = LOCAL_FIXTURE;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
          record_equals(store, book, LOCAL_FIXTURE) && get_calls == 1,
          "fresh GET uses current C, not a prior conflict B");
    reset_fake(); remote_location = LOCAL_FIXTURE;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
          save_calls == 1 && get_calls == 1,
          "equal A/C still GETs and persists by explicit authorization");
    reset_fake(); save_fault = 1;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_LOCAL_FAILURE &&
          result.pull.local_save_attempted && !result.pull.local_saved &&
          result.pull.local_save_result == CN_PROGRESS_IO_ERROR &&
          result.local_mutation == CN_KOSYNC_MUTATION_POSSIBLE &&
          record_equals(store, book, LOCAL_FIXTURE) && !put_calls,
          "failed local save preserves prior final and reports possible mutation");
    reset_fake(); get_mode = CN_KOSYNC_AUTH_FAILED;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_AUTH_REQUIRED && !save_calls &&
          !put_calls, "GET 401 auth failure does not save");
    reset_fake(); time_mode = CN_TIME_TIMEOUT;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_TRUSTED_TIME_UNAVAILABLE &&
          !dns_calls && !get_calls, "trusted time fails before DNS/GET");
    reset_fake(); dns_mode = CN_DNS_TIMEOUT;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_CONNECTIVITY_FAILURE && !get_calls,
          "DNS timeout fails before GET");
    reset_fake(); get_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_SECURITY_FAILURE &&
          result.pull.kosync_outcome.transport_result == transport_mode,
          "TLS hostname mismatch remains security failure");
    reset_fake(); get_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_TLS_TRUST_FAILED;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_SECURITY_FAILURE,
          "TLS trust failure remains security failure");
    reset_fake(); get_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_RECV_TIMEOUT;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_CONNECTIVITY_FAILURE &&
          !save_calls, "GET receive timeout has no local mutation");
    reset_fake(); get_mode = CN_KOSYNC_BAD_JSON;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_SERVICE_FAILURE && !save_calls,
          "malformed JSON no local mutation");
    reset_fake(); get_mode = CN_KOSYNC_BAD_PROTOCOL;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_SERVICE_FAILURE && !save_calls,
          "bad remote document/protocol no local mutation");
    reset_fake(); get_mode = CN_KOSYNC_HTTP_ERROR;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_SERVICE_FAILURE &&
          result.pull.kosync_outcome.http_status == 404 && !save_calls,
          "HTTP 404 is not remote-missing");
    reset_fake(); remote_location = REMOTE_FIXTURE;
    result = invoke(store, book);
    check(result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
          record_equals(store, book, REMOTE_FIXTURE),
          "repeat after errors persists current remote fixture");
    {
        cn_book_identity id;
        char file[CN_STORAGE_PATH_CAPACITY];
        int fd;
        if (cn_book_identity_from_path(&id, book) != 0 ||
            snprintf(file, sizeof file, "%s/%s.progress", directory,
                     cn_book_identity_token(&id)) >= (int)sizeof file ||
            unlink(file) != 0) ++failures;
        reset_fake();
        result = invoke(store, book);
        check(result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
              record_equals(store, book, REMOTE_FIXTURE) &&
              get_calls == 1 && save_calls == 1 && !put_calls,
              "remote B creates missing local record without PUT");
        fd = open(file, O_WRONLY | O_TRUNC);
        if (fd < 0) ++failures;
        else {
            if (write(fd, "bad", 3) != 3) ++failures;
            if (close(fd) != 0) ++failures;
        }
        reset_fake(); remote_location = LOCAL_FIXTURE;
        result = invoke(store, book);
        check(result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
              record_equals(store, book, LOCAL_FIXTURE) && !put_calls,
              "authorized fresh remote overwrites corrupt local record");
    }
    /* Application apply uses a Reader-generated, known-resolvable remote B. */
    {
        cn_reader_config cfg;
        memset(&cfg, 0, sizeof cfg); cfg.font_path = font;
        library = cn_library_new(); ui = cn_ui_init();
        if (!library || !ui || cn_library_add(library, book, "synthetic.epub", CN_BOOK_EPUB) < 0 ||
            cn_ui_set_reader(ui, &cfg) != 0 || cn_ui_set_library(ui, library) != 1)
            ++failures;
        else {
            memset(&event, 0, sizeof event);
            event.type = CN_INPUT_TOUCH_UP; event.x = 200; event.y = 320;
            (void)cn_ui_handle(ui, &event);
            event.y = CN_UI_LIB_ROW_TOP + 10;
            (void)cn_ui_handle(ui, &event);
            event.type = CN_INPUT_PAGE_NEXT;
            (void)cn_ui_handle(ui, &event);
            if (cn_ui_reader_get_position(ui, &position_b) != 0 ||
                !position_b.location) ++failures;
            else {
                memset(&event, 0, sizeof event); event.type = CN_INPUT_PAGE_NEXT;
                (void)cn_ui_handle(ui, &event);
                reset_fake(); remote_location = position_b.location;
                result = invoke(store, book);
                check(result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
                      record_equals(store, book, position_b.location) &&
                      get_calls == 1 && !put_calls,
                      "fresh GET persists the same B later applied to Reader");
                rc = get_calls;
                applied = cn_reader_sync_apply_persisted(ui, store);
                {
                    cn_reader_position recaptured;
                    cn_reader_position_init(&recaptured);
                    check(applied.phase == CN_READER_SYNC_IMPORT_APPLIED &&
                          cn_ui_reader_get_position(ui, &recaptured) == 0 &&
                          recaptured.location &&
                          !strcmp(recaptured.location, position_b.location),
                          "real Reader recaptures pulled B fixture");
                    cn_reader_position_clear(&recaptured);
                }
                check(applied.phase == CN_READER_SYNC_IMPORT_APPLIED &&
                      applied.redraw_needed && get_calls == rc && !put_calls,
                      "persisted B applied to already-open Reader without GET/PUT");
            }
        }
    }
    cn_reader_position_clear(&position_b);
    cn_ui_free(ui); cn_library_free(library);
    cn_progress_store_close(store);
    fake = 0;
    printf("REMOTE PULL SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int parse_number(const char *text, unsigned long max,
                        unsigned long *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !text[0] || text[0] == '-') return 0;
    errno = 0; parsed = strtoul(text, &end, 10);
    if (errno || *end || parsed > max) return 0;
    *value = parsed;
    return 1;
}
static int card_directory(const char *path, const struct stat *root)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           st.st_dev == root->st_dev;
}
static cn_ui *open_reader(const char *font, const char *book, cn_library **lib)
{
    cn_reader_config cfg;
    cn_ui *ui = cn_ui_init();
    cn_library *library = cn_library_new();
    cn_input_ev event;
    memset(&cfg, 0, sizeof cfg); cfg.font_path = font;
    if (!ui || !library || cn_library_add(library, book, "synthetic.epub", CN_BOOK_EPUB) < 0 ||
        cn_ui_set_reader(ui, &cfg) != 0 || cn_ui_set_library(ui, library) != 1) {
        cn_ui_free(ui); cn_library_free(library); return NULL;
    }
    *lib = library;
    memset(&event, 0, sizeof event);
    event.type = CN_INPUT_TOUCH_UP; event.x = 200; event.y = 320;
    (void)cn_ui_handle(ui, &event);
    event.y = CN_UI_LIB_ROW_TOP + 10;
    (void)cn_ui_handle(ui, &event);
    if (cn_ui_get_state(ui) != CN_UI_READER) {
        cn_ui_free(ui); cn_library_free(library); *lib = NULL; return NULL;
    }
    return ui;
}
static void go_pages(cn_ui *ui, int count)
{
    cn_input_ev event;
    memset(&event, 0, sizeof event); event.type = CN_INPUT_PAGE_NEXT;
    while (count-- > 0) (void)cn_ui_handle(ui, &event);
}
static int ui_matches(cn_ui *ui, const char *location)
{
    cn_reader_position captured;
    int ok;
    cn_reader_position_init(&captured);
    ok = cn_ui_reader_get_position(ui, &captured) == 0 &&
         location && captured.location && !strcmp(captured.location, location);
    cn_reader_position_clear(&captured);
    return ok;
}

/* These later operator-run phases are distinct: seed-remote uses a controlled
 * mock SETUP PUT; the actual pull never calls PUT. Count only pull deltas. */
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
    cn_sync_controller_config config;
    cn_sync_pull_result result;
    cn_dns_config dns;
    cn_time_config time;
    cn_tls_config tls;
    cn_kosync_client client, routed;
    cn_kosync_progress upload;
    cn_kosync_outcome outcome;
    cn_dns_answer answer;
    cn_time_sample sample;
    cn_koreader_document_id remote_id;
    cn_book_identity local_id;
    cn_progress_record old_record, new_record, seed_record;
    cn_reader_position a, b;
    cn_ui *ui = NULL;
    cn_library *library = NULL;
    cn_canvas *canvas = NULL;
    cn_reader_sync_result applied;
    struct stat root_st;
    char gate[CN_STORAGE_PATH_CAPACITY], config_dir[CN_STORAGE_PATH_CAPACITY];
    char state_dir[CN_STORAGE_PATH_CAPACITY], progress_dir[CN_STORAGE_PATH_CAPACITY];
    unsigned long major_number, minor_number, dns_port, time_port;
    const char *phase, *scenario, *account;
    int rc = 1, saved_stderr = -1, sink = -1;
    int unchanged = 0, remote_matches = 0, rendered = 0, reader_unchanged;

    if (argc != 15) return 2;
    phase = argv[2]; scenario = argv[3];
    if (strcmp(phase, "seed-local") && strcmp(phase, "seed-remote") &&
        strcmp(phase, "pull") && strcmp(phase, "apply")) return 2;
    if (strcmp(scenario, "present") && strcmp(scenario, "missing")) return 2;
    if (!strcmp(phase, "seed-remote") && strcmp(scenario, "present")) return 2;
    account = !strcmp(scenario, "present") ? "integration-local-only" :
                                                  "integration-both-missing";
    if (!parse_number(argv[6], 0xffffffffUL, &major_number) ||
        !parse_number(argv[7], 0xffffffffUL, &minor_number) ||
        !parse_number(argv[11], 65535, &dns_port) || !dns_port ||
        !parse_number(argv[13], 65535, &time_port) || !time_port) return 2;
    candidate.root = argv[5]; candidate.mountpoint = argv[4];
    candidate.expected_major = (unsigned)major_number;
    candidate.expected_minor = (unsigned)minor_number;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) != CN_PLATFORM_STORAGE_OK) {
        puts("REMOTE PULL GATE storage=unverified persistence=not-attempted network=not-attempted");
        return 1;
    }
    if (stat(verified.root, &root_st) != 0 ||
        snprintf(gate, sizeof gate, "%s/controller-%s", verified.root,
                 !strcmp(scenario, "present") ? "upload" : "no-state") >=
            (int)sizeof gate || !card_directory(gate, &root_st) ||
        cn_storage_layout_init(&layout, gate, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG,
                               config_dir, sizeof config_dir) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                               progress_dir, sizeof progress_dir) != CN_STORAGE_OK ||
        snprintf(state_dir, sizeof state_dir, "%s/state", gate) >=
            (int)sizeof state_dir || !card_directory(config_dir, &root_st) ||
        !card_directory(progress_dir, &root_st) ||
        !card_directory(state_dir, &root_st) ||
        cn_settings_store_init(&settings_store, config_dir, NULL) != CN_SETTINGS_OK ||
        cn_credential_store_init(&credential_store, state_dir, NULL) != CN_CREDENTIAL_OK ||
        cn_progress_store_open(&store, progress_dir) != CN_PROGRESS_OK) return 1;

    cn_progress_record_init(&old_record);
    cn_progress_record_init(&new_record);
    cn_progress_record_init(&seed_record);
    cn_reader_position_init(&a); cn_reader_position_init(&b);
    cn_kosync_progress_init(&upload);
    memset(&credentials, 0, sizeof credentials);
    memset(&client, 0, sizeof client); memset(&routed, 0, sizeof routed);
    /* Reader logs a book path on stderr; physical gate exposes status only. */
    saved_stderr = dup(STDERR_FILENO);
    sink = open("/dev/null", O_WRONLY);
    if (saved_stderr < 0 || sink < 0 || dup2(sink, STDERR_FILENO) < 0)
        goto done;
    close(sink); sink = -1;
    if (cn_settings_load(&settings_store, &settings, NULL) != CN_SETTINGS_OK ||
        !settings.kosync_enabled ||
        cn_credential_store_load(&credential_store, &credentials, NULL) != CN_CREDENTIAL_OK ||
        strcmp(credentials.username, account) ||
        strcmp(credentials.userkey, MOCK_KEY)) goto done;
    cn_credentials_clear(&credentials);
    ui = open_reader(argv[8], argv[9], &library);
    if (!ui || cn_book_identity_from_path(&local_id, argv[9]) != 0) goto done;
    go_pages(ui, 5);
    if (cn_ui_reader_get_position(ui, &a) != 0 || !a.location) goto done;
    if (!strcmp(phase, "seed-local")) {
        seed_record.position = a;
        rc = __real_cn_progress_store_save(store, &local_id, &seed_record) == CN_PROGRESS_OK
                 ? 0 : 1;
        seed_record.position.location = NULL;
        puts(rc ? "REMOTE PULL GATE seed-local=failed" :
                  "REMOTE PULL GATE seed-local=ok");
        goto done;
    }
    if (!strcmp(phase, "apply")) {
        applied = cn_reader_sync_apply_persisted(ui, store);
        if (cn_progress_store_load(store, &local_id, &new_record) == CN_PROGRESS_OK)
            remote_matches = ui_matches(ui, new_record.position.location);
        canvas = cn_canvas_create(CN_READER_W, CN_READER_H);
        if (canvas && applied.redraw_needed) {
            const uint16_t *pixels;
            size_t i;
            cn_ui_render(ui, canvas, NULL);
            pixels = (const uint16_t *)cn_canvas_pixels(canvas);
            if (pixels)
                for (i = 0; i < (size_t)CN_READER_W * CN_READER_H; ++i)
                    if (pixels[i] != CN_COLOR_WHITE) { rendered = 1; break; }
        }
        printf("REMOTE PULL GATE apply=%s reload=%s redraw-needed=%d "
               "recapture-matched=%s render=%s network=not-attempted\n",
               cn_reader_sync_phase_name(applied.phase),
               cn_progress_result_name(applied.import_reload_result),
               applied.redraw_needed, remote_matches ? "yes" : "no",
               rendered ? "ok" : "failed");
        rc = applied.phase == CN_READER_SYNC_IMPORT_APPLIED &&
             remote_matches && rendered ? 0 : 1;
        goto done;
    }
    if (cn_progress_store_load(store, &local_id, &old_record) != CN_PROGRESS_OK ||
        !old_record.position.location ||
        strcmp(old_record.position.location, a.location)) goto done;
    memset(&dns, 0, sizeof dns);
    dns.servers[0] = argv[10]; dns.server_count = 1; dns.port = (unsigned)dns_port;
    memset(&time, 0, sizeof time);
    time.servers[0] = argv[12]; time.server_count = 1; time.port = (unsigned)time_port;
    memset(&tls, 0, sizeof tls); tls.ca_path = argv[14];
    memset(&config, 0, sizeof config);
    config.settings_store = &settings_store;
    config.credential_store = &credential_store;
    config.progress_store = store; config.document_path = argv[9];
    config.dns = &dns; config.time = &time; config.tls = &tls;
    config.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    config.device_id = NULL; /* GET does not send or require an ID. */
    if (!strcmp(phase, "seed-remote")) {
        cn_input_ev event;
        const char *id;
        int i;
        memset(&event, 0, sizeof event); event.type = CN_INPUT_PAGE_PREV;
        for (i = 0; i < 4; ++i) (void)cn_ui_handle(ui, &event);
        if (cn_ui_reader_get_position(ui, &b) != 0 || !b.location ||
            strcmp(b.location, a.location) == 0 ||
            cn_book_identity_koreader_binary(argv[9], &remote_id) !=
                CN_KOREADER_IDENTITY_OK) goto done;
        id = cn_koreader_document_id_text(&remote_id);
        if (!id || cn_kosync_client_init(&client, settings.kosync_base_url,
                                         account, MOCK_KEY) != CN_KOSYNC_OK ||
            !client.use_tls || cn_timesimple_sync(&time, &sample) != CN_TIME_OK ||
            cn_dnssimple_resolve_a(&dns, client.host, &answer) != CN_DNS_OK ||
            answer.count == 0) goto done;
        routed = client;
        if (cn_kosync_client_set_tls(&routed, &tls, answer.ipv4[0]) != CN_KOSYNC_OK ||
            cn_kosync_progress_set(&upload, id, b.location, b.progress_10000,
                                    "synthetic-seed-device", "synthetic-seed-id") != CN_KOSYNC_OK ||
            cn_kosync_put_progress(&routed, &upload, NULL, &outcome) != CN_KOSYNC_OK)
            goto done;
        puts("REMOTE PULL GATE seed-remote=ok setup-put=1");
        rc = 0; goto done;
    }
    result = cn_sync_pull_remote_current_book(&config);
    unchanged = cn_progress_store_load(store, &local_id, &new_record) == CN_PROGRESS_OK &&
                new_record.position.location &&
                !strcmp(new_record.position.location, old_record.position.location);
    reader_unchanged = ui_matches(ui, a.location);
    if (!strcmp(scenario, "present") && new_record.position.location) {
        cn_input_ev event;
        int i;
        memset(&event, 0, sizeof event); event.type = CN_INPUT_PAGE_PREV;
        for (i = 0; i < 4; ++i) (void)cn_ui_handle(ui, &event);
        remote_matches = ui_matches(ui, new_record.position.location);
    }
    printf("REMOTE PULL GATE outcome=%s stage=%s get-attempted=%d "
           "local-save-attempted=%d local-save=%s local-mutation=%s "
           "remote-mutation=%s local-unchanged=%s remote-matched=%s "
           "reader-unchanged=%s\n",
           cn_sync_pull_outcome_name(result.outcome),
           cn_sync_pull_stage_name(result.stage), result.pull.get_attempted,
           result.pull.local_save_attempted,
           cn_progress_result_name(result.pull.local_save_result),
           cn_kosync_mutation_state_name(result.local_mutation),
           cn_kosync_mutation_state_name(result.remote_mutation),
           unchanged ? "yes" : "no", remote_matches ? "yes" : "no",
           reader_unchanged ? "yes" : "no");
    if (!strcmp(scenario, "present"))
        rc = result.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
             result.local_mutation == CN_KOSYNC_MUTATION_CONFIRMED &&
             remote_matches && !unchanged && reader_unchanged ? 0 : 1;
    else
        rc = result.outcome == CN_SYNC_PULL_REMOTE_MISSING_OUTCOME &&
             unchanged && reader_unchanged && !result.pull.local_save_attempted
                 ? 0 : 1;
    if (!result.pull.get_attempted || result.remote_mutation != CN_KOSYNC_MUTATION_NONE)
        rc = 1;
done:
    if (saved_stderr >= 0) {
        fflush(stderr);
        (void)dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    if (sink >= 0) close(sink);
    cn_kosync_progress_clear(&upload);
    clear_bytes(&client, sizeof client); clear_bytes(&routed, sizeof routed);
    cn_credentials_clear(&credentials);
    cn_reader_position_clear(&a); cn_reader_position_clear(&b);
    cn_progress_record_clear(&old_record); cn_progress_record_clear(&new_record);
    cn_progress_record_clear(&seed_record);
    cn_canvas_free(canvas);
    cn_ui_free(ui); cn_library_free(library);
    cn_progress_store_close(store);
    if (rc != 0) puts("REMOTE PULL GATE result=failed");
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--smoke") == 0)
        return smoke(argv[2], argv[3], argv[4]);
    if (argc >= 2 && strcmp(argv[1], "--physical") == 0)
        return physical(argc, argv);
    fputs("usage: remote-pull-test --smoke <font> <epub> <existing-directory> | --physical <seed-local|seed-remote|pull|apply> <present|missing> <mount> <root> <major> <minor> <font> <epub> <dns-ip> <dns-port> <sntp-ip> <sntp-port> <ca>\n", stderr);
    return 2;
}
