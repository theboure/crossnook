/* Synthetic PUT-only host matrix and verified-card, status-only diagnostic. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/reader_sync.h"
#include "platform/storage_verify.h"
#include "storage/storage_layout.h"

#define DUMMY_USER "crossnook-synthetic-push-account"
#define DUMMY_KEY "crossnook-synthetic-push-key"
#define MOCK_KEY "dfb450efddbb5387197c84460623675b"
#define LOCAL_A "/body/DocFragment[1]/body/p[1]/text().0"
#define REMOTE_B "/body/DocFragment[1]/body/p[3]/text().5"

static int fake, failures;
static int enabled, invalid_url, invalid_device_name, server_present, server_applies;
static int settings_calls, credential_calls, clear_calls, time_calls, dns_calls;
static int put_calls, get_calls, normal_calls, pull_calls, save_calls;
static int server_progress;
static char server_position[CN_KOSYNC_POSITION_MAX + 1];
static cn_settings_result settings_mode;
static cn_credential_result credential_mode;
static cn_time_result time_mode;
static cn_dns_result dns_mode;
static cn_kosync_result put_mode;
static cn_netsimple_result transport_mode;

cn_settings_result __real_cn_settings_load(const cn_settings_store *, cn_settings *, int *);
cn_credential_result __real_cn_credential_store_load(const cn_credential_store *, cn_credentials *, int *);
void __real_cn_credentials_clear(cn_credentials *);
cn_time_result __real_cn_timesimple_sync(const cn_time_config *, cn_time_sample *);
cn_dns_result __real_cn_dnssimple_resolve_a(const cn_dns_config *, const char *, cn_dns_answer *);
cn_kosync_result __real_cn_kosync_put_progress(const cn_kosync_client *, const cn_kosync_progress *, long long *, cn_kosync_outcome *);
cn_kosync_result __real_cn_kosync_get_progress(const cn_kosync_client *, const char *, cn_kosync_progress *, cn_kosync_outcome *);
cn_kosync_sync_status __real_cn_kosync_sync_once(const cn_kosync_sync_config *, cn_kosync_sync_result *);
cn_sync_controller_result __real_cn_sync_current_book(const cn_sync_controller_config *);
cn_sync_pull_result __real_cn_sync_pull_remote_current_book(const cn_sync_controller_config *);
cn_progress_result __real_cn_progress_store_save(cn_progress_store *, const cn_book_identity *, const cn_progress_record *);

cn_settings_result __wrap_cn_settings_load(const cn_settings_store *store,
                                           cn_settings *out, int *error)
{
    if (!fake) return __real_cn_settings_load(store, out, error);
    ++settings_calls;
    cn_settings_defaults(out);
    if (settings_mode == CN_SETTINGS_OK) {
        out->kosync_enabled = enabled;
        strcpy(out->kosync_base_url, invalid_url ? "http://sync.synthetic.invalid/" :
                                                  "https://sync.synthetic.invalid/");
        if (invalid_device_name) out->kosync_device_name[0] = 0;
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
    if (fake) ++clear_calls;
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
    const cn_dns_config *config, const char *host, cn_dns_answer *answer)
{
    if (!fake) return __real_cn_dnssimple_resolve_a(config, host, answer);
    (void)config;
    ++dns_calls;
    if (!host || strcmp(host, "sync.synthetic.invalid")) ++failures;
    memset(answer, 0, sizeof *answer);
    if (dns_mode == CN_DNS_OK) {
        answer->count = 1;
        strcpy(answer->ipv4[0], "127.0.0.2");
    }
    return dns_mode;
}
cn_kosync_result __wrap_cn_kosync_put_progress(
    const cn_kosync_client *client, const cn_kosync_progress *progress,
    long long *timestamp, cn_kosync_outcome *outcome)
{
    size_t length;
    if (!fake) return __real_cn_kosync_put_progress(client, progress, timestamp, outcome);
    ++put_calls;
    if (!client || !client->use_tls || !client->tls ||
        strcmp(client->host, "sync.synthetic.invalid") ||
        strcmp(client->connect_host, "127.0.0.2") ||
        strcmp(client->username, DUMMY_USER) ||
        strcmp(client->userkey, DUMMY_KEY) ||
        !progress || !progress->logical_position ||
        strcmp(progress->device, "CrossNook") ||
        strcmp(progress->device_id, "synthetic-runtime-id")) ++failures;
    if (outcome) {
        outcome->transport_result = put_mode == CN_KOSYNC_TRANSPORT_ERROR
                                        ? transport_mode : CN_NETSIMPLE_OK;
        outcome->http_status = put_mode == CN_KOSYNC_AUTH_FAILED ? 401 :
                               put_mode == CN_KOSYNC_HTTP_ERROR ? 503 :
                               put_mode == CN_KOSYNC_TRANSPORT_ERROR ? 0 : 200;
    }
    if (server_applies || put_mode == CN_KOSYNC_OK) {
        length = strlen(progress->logical_position);
        if (length > CN_KOSYNC_POSITION_MAX) ++failures;
        else {
            memcpy(server_position, progress->logical_position, length + 1);
            server_present = 1;
            server_progress = progress->progress_10000;
        }
    }
    return put_mode;
}
cn_kosync_result __wrap_cn_kosync_get_progress(
    const cn_kosync_client *client, const char *document,
    cn_kosync_progress *progress, cn_kosync_outcome *outcome)
{
    if (!fake) return __real_cn_kosync_get_progress(client, document, progress, outcome);
    (void)client; (void)document; (void)progress; (void)outcome;
    ++get_calls; ++failures;
    return CN_KOSYNC_INVALID;
}
cn_kosync_sync_status __wrap_cn_kosync_sync_once(
    const cn_kosync_sync_config *config, cn_kosync_sync_result *result)
{
    if (!fake) return __real_cn_kosync_sync_once(config, result);
    ++normal_calls; ++failures;
    return CN_KOSYNC_SYNC_STATUS_INVALID;
}
cn_sync_controller_result __wrap_cn_sync_current_book(
    const cn_sync_controller_config *config)
{
    if (!fake) return __real_cn_sync_current_book(config);
    ++normal_calls; ++failures;
    return (cn_sync_controller_result){0};
}
cn_sync_pull_result __wrap_cn_sync_pull_remote_current_book(
    const cn_sync_controller_config *config)
{
    if (!fake) return __real_cn_sync_pull_remote_current_book(config);
    ++pull_calls; ++failures;
    return (cn_sync_pull_result){0};
}
cn_progress_result __wrap_cn_progress_store_save(
    cn_progress_store *store, const cn_book_identity *id,
    const cn_progress_record *record)
{
    if (fake) { ++save_calls; ++failures; return CN_PROGRESS_INVALID; }
    return __real_cn_progress_store_save(store, id, record);
}

static void check(int condition, const char *description)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", description);
    fflush(stdout);
    if (!condition) ++failures;
}
static void reset_fake(void)
{
    fake = enabled = 1;
    invalid_url = invalid_device_name = server_applies = 0;
    settings_mode = CN_SETTINGS_OK;
    credential_mode = CN_CREDENTIAL_OK;
    time_mode = CN_TIME_OK;
    dns_mode = CN_DNS_OK;
    put_mode = CN_KOSYNC_OK;
    transport_mode = CN_NETSIMPLE_OK;
    settings_calls = credential_calls = clear_calls = time_calls = dns_calls = 0;
    put_calls = get_calls = normal_calls = pull_calls = save_calls = 0;
}
static int seed(cn_progress_store *store, const char *book,
                const char *location, int percentage)
{
    cn_book_identity id;
    cn_progress_record record;
    cn_progress_result result;
    if (cn_book_identity_from_path(&id, book) != 0) return 0;
    cn_progress_record_init(&record);
    record.position.location = (char *)location;
    record.position.progress_10000 = percentage;
    result = __real_cn_progress_store_save(store, &id, &record);
    record.position.location = NULL;
    return result == CN_PROGRESS_OK;
}
static int loaded_equals(cn_progress_store *store, const char *book,
                         const char *location, int percentage)
{
    cn_book_identity id;
    cn_progress_record record;
    int equal;
    if (cn_book_identity_from_path(&id, book) != 0) return 0;
    cn_progress_record_init(&record);
    equal = cn_progress_store_load(store, &id, &record) == CN_PROGRESS_OK &&
            record.position.location && !strcmp(record.position.location, location) &&
            record.position.progress_10000 == percentage;
    cn_progress_record_clear(&record);
    return equal;
}
static cn_sync_push_result invoke(cn_progress_store *store, const char *book,
                                  const char *device_id)
{
    cn_sync_controller_config config;
    cn_dns_config dns = {{"127.0.0.2"}, 1, 53, 200};
    cn_time_config time = {{"127.0.0.2"}, 1, 123, 200};
    cn_tls_config tls = {"/tmp/synthetic-ca", NULL, NULL};
    memset(&config, 0, sizeof config);
    config.settings_store = (const cn_settings_store *)&config;
    config.credential_store = (const cn_credential_store *)&config;
    config.progress_store = store; config.document_path = book;
    config.device_id = device_id;
    config.dns = &dns; config.time = &time; config.tls = &tls;
    config.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    return cn_sync_push_local_current_book(&config);
}
static int no_other_actions(void)
{
    return !get_calls && !normal_calls && !pull_calls && !save_calls;
}
static int ui_matches(cn_ui *ui, const char *location);

static int smoke(const char *font, const char *book, const char *directory)
{
    cn_progress_store *store = NULL;
    cn_sync_push_result result;
    cn_book_identity id;
    char file[CN_STORAGE_PATH_CAPACITY];
    cn_reader_config reader_config;
    cn_library *library = NULL;
    cn_ui *ui = NULL;
    cn_input_ev event;
    cn_reader_position captured, after;
    cn_progress_record record;
    char *long_location = NULL;
    int fd;
    cn_reader_position_init(&captured);
    cn_reader_position_init(&after);
    if (cn_progress_store_open(&store, directory) != CN_PROGRESS_OK ||
        cn_book_identity_from_path(&id, book) != 0 ||
        snprintf(file, sizeof file, "%s/%s.progress", directory,
                 cn_book_identity_token(&id)) >= (int)sizeof file) return 1;

    reset_fake(); settings_mode = CN_SETTINGS_MISSING;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_DISABLED_OUTCOME &&
          !credential_calls && !time_calls && !dns_calls && !put_calls,
          "missing Settings disabled before credentials and network");
    reset_fake(); enabled = 0;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_DISABLED_OUTCOME && !credential_calls,
          "explicit disabled short-circuits");
    reset_fake(); settings_mode = CN_SETTINGS_CORRUPT;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.stage == CN_SYNC_PUSH_SETTINGS_FAILED && !credential_calls,
          "corrupt Settings fail before credentials");
    reset_fake(); credential_mode = CN_CREDENTIAL_MISSING;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_AUTH_REQUIRED && !put_calls &&
          clear_calls == 1, "missing credentials require explicit action");
    reset_fake(); credential_mode = CN_CREDENTIAL_CORRUPT;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_CONFIGURATION_FAILURE && !put_calls,
          "corrupt credentials fail before network");
    reset_fake(); invalid_url = 1;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.stage == CN_SYNC_PUSH_CONFIG_FAILED && !put_calls,
          "plaintext URL rejected before network");
    reset_fake(); invalid_device_name = 1;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.stage == CN_SYNC_PUSH_CONFIG_FAILED &&
          !time_calls && !dns_calls && !put_calls,
          "invalid Settings device name rejected before network");
    reset_fake();
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_LOCAL_MISSING_OUTCOME &&
          result.push.local_load_result == CN_PROGRESS_MISSING &&
          !time_calls && !dns_calls && !put_calls && no_other_actions(),
          "missing local record never reaches network");
    fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) ++failures;
    else {
        if (write(fd, "bad", 3) != 3) ++failures;
        if (close(fd) != 0) ++failures;
    }
    reset_fake();
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_LOCAL_FAILURE &&
          result.push.local_load_result == CN_PROGRESS_CORRUPT &&
          !time_calls && !dns_calls && !put_calls,
          "corrupt local record never reaches network");
    check(seed(store, book, LOCAL_A, -1), "valid persisted unknown percentage fixture");
    reset_fake();
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_LOCAL_UNSUPPORTED_OUTCOME &&
          !time_calls && !dns_calls && !put_calls && no_other_actions(),
          "local percentage -1 unsupported before network");
    check(seed(store, book, LOCAL_A, 3210), "persist local A fixture");
    reset_fake();
    result = invoke(store, book, NULL);
    check(result.stage == CN_SYNC_PUSH_CONFIG_FAILED && !time_calls && !put_calls,
          "missing runtime device ID rejected before network");
    reset_fake();
    result = invoke(store, book, "bad\nidentifier");
    check(result.outcome == CN_SYNC_PUSH_CONFIGURATION_FAILURE &&
          !time_calls && !dns_calls && !put_calls,
          "invalid runtime device ID rejected by existing conversion");
    reset_fake(); time_mode = CN_TIME_TIMEOUT;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_TRUSTED_TIME_UNAVAILABLE &&
          !dns_calls && !put_calls && !server_present,
          "trusted-time failure before DNS/PUT");
    reset_fake(); dns_mode = CN_DNS_TIMEOUT;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_CONNECTIVITY_FAILURE && !put_calls,
          "DNS failure before PUT");

    strcpy(server_position, REMOTE_B); server_present = 1; server_progress = 6543;
    reset_fake();
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME &&
          result.push.put_invoked && result.push.remote_uploaded &&
          result.remote_mutation == CN_KOSYNC_MUTATION_CONFIRMED &&
          result.local_mutation == CN_KOSYNC_MUTATION_NONE &&
          put_calls == 1 && no_other_actions() &&
          !strcmp(server_position, LOCAL_A) && server_progress == 3210 &&
          loaded_equals(store, book, LOCAL_A, 3210),
          "remote B overwritten by persisted A, PUT=1 GET=0");
    server_present = 0; server_position[0] = 0;
    reset_fake();
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME && put_calls == 1 &&
          no_other_actions() && server_present && !strcmp(server_position, LOCAL_A),
          "remote missing created by direct PUT, no GET");
    reset_fake();
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME && put_calls == 1 &&
          no_other_actions() && result.remote_mutation == CN_KOSYNC_MUTATION_CONFIRMED,
          "repeated equal A still performs acknowledged PUT");

    reset_fake(); put_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_CONNECT_REFUSED;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_CONNECTIVITY_FAILURE && put_calls == 1 &&
          result.remote_mutation == CN_KOSYNC_MUTATION_NONE,
          "proven pre-send connect failure remote mutation none");
    reset_fake(); put_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_TLS_TRUST_FAILED;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_SECURITY_FAILURE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_NONE,
          "TLS trust failure before HTTP send remote mutation none");
    reset_fake(); put_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_SECURITY_FAILURE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_NONE,
          "TLS hostname failure before HTTP send remote mutation none");
    reset_fake(); put_mode = CN_KOSYNC_AUTH_FAILED;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_AUTH_REQUIRED &&
          result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE,
          "PUT 401 response is conservatively possible");
    reset_fake(); put_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_SEND_ERROR;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_CONNECTIVITY_FAILURE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE,
          "ambiguous send failure remote mutation possible");
    reset_fake(); put_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_TLS_RECV_TIMEOUT;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE &&
          put_calls == 1 && no_other_actions(),
          "receive timeout does not retry or claim remote none");
    reset_fake(); put_mode = CN_KOSYNC_HTTP_ERROR;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_SERVICE_FAILURE &&
          result.push.kosync_outcome.http_status == 503 &&
          result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE,
          "HTTP error after attempted PUT remains possible");
    reset_fake(); put_mode = CN_KOSYNC_BAD_JSON;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_SERVICE_FAILURE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE,
          "malformed PUT acknowledgement remains possible");
    reset_fake(); put_mode = CN_KOSYNC_BAD_PROTOCOL;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_SERVICE_FAILURE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE,
          "mismatched PUT acknowledgement remains possible");
    reset_fake(); put_mode = CN_KOSYNC_NO_MEMORY;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_INTERNAL_FAILURE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE,
          "post-PUT allocation failure not mistaken for pre-send");
    strcpy(server_position, REMOTE_B); server_present = 1;
    reset_fake(); put_mode = CN_KOSYNC_TRANSPORT_ERROR;
    transport_mode = CN_NETSIMPLE_TLS_RECV_TIMEOUT;
    server_applies = 1;
    result = invoke(store, book, "synthetic-runtime-id");
    check(result.outcome == CN_SYNC_PUSH_CONNECTIVITY_FAILURE &&
          result.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE &&
          put_calls == 1 && no_other_actions() &&
          !strcmp(server_position, LOCAL_A) && loaded_equals(store, book, LOCAL_A, 3210),
          "server applied PUT then lost response: POSSIBLE, no retry");

    long_location = (char *)malloc(CN_KOSYNC_POSITION_MAX + 1);
    if (!long_location) ++failures;
    else {
        memset(long_location, 'x', CN_KOSYNC_POSITION_MAX);
        long_location[0] = '/';
        long_location[CN_KOSYNC_POSITION_MAX] = 0;
        check(seed(store, book, long_location, 10000),
              "65536-byte valid local position at upload boundary");
        reset_fake();
        result = invoke(store, book, "synthetic-runtime-id");
        check(result.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME &&
              put_calls == 1 && strlen(server_position) == CN_KOSYNC_POSITION_MAX,
              "existing KOSync conversion accepts exact position limit");
        free(long_location);
    }

    /* Diagnostic application composition: capture, save, then push persisted A. */
    memset(&reader_config, 0, sizeof reader_config); reader_config.font_path = font;
    library = cn_library_new(); ui = cn_ui_init();
    if (!library || !ui || cn_library_add(library, book, "synthetic.epub", CN_BOOK_EPUB) < 0 ||
        cn_ui_set_reader(ui, &reader_config) != 0 ||
        cn_ui_set_library(ui, library) != 1) ++failures;
    else {
        memset(&event, 0, sizeof event); event.type = CN_INPUT_TOUCH_UP;
        event.x = 200; event.y = 320; (void)cn_ui_handle(ui, &event);
        event.y = CN_UI_LIB_ROW_TOP + 10; (void)cn_ui_handle(ui, &event);
        event.type = CN_INPUT_PAGE_NEXT; (void)cn_ui_handle(ui, &event);
        if (cn_ui_reader_get_position(ui, &captured) != 0 || !captured.location)
            ++failures;
        else {
            cn_progress_record_init(&record);
            record.position = captured;
            check(__real_cn_progress_store_save(store, &id, &record) == CN_PROGRESS_OK,
                  "Reader A captured and saved before push");
            record.position.location = NULL;
            strcpy(server_position, REMOTE_B); server_present = 1;
            reset_fake();
            result = invoke(store, book, "synthetic-runtime-id");
            check(result.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME &&
                  result.remote_mutation == CN_KOSYNC_MUTATION_CONFIRMED &&
                  loaded_equals(store, book, captured.location, captured.progress_10000) &&
                  !strcmp(server_position, captured.location) &&
                  cn_ui_reader_get_position(ui, &after) == 0 && after.location &&
                  !strcmp(after.location, captured.location) &&
                  put_calls == 1 && no_other_actions(),
                  "Reader A -> persisted A -> remote A; Reader stays A, no sync");
            cn_reader_position_clear(&after);
            memset(&event, 0, sizeof event); event.type = CN_INPUT_PAGE_NEXT;
            (void)cn_ui_handle(ui, &event);
            if (cn_ui_reader_get_position(ui, &after) != 0 || !after.location ||
                !strcmp(after.location, captured.location)) ++failures;
            else {
                strcpy(server_position, REMOTE_B);
                reset_fake();
                result = invoke(store, book, "synthetic-runtime-id");
                check(result.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME &&
                      !strcmp(server_position, captured.location) &&
                      loaded_equals(store, book, captured.location,
                                    captured.progress_10000) &&
                      ui_matches(ui, after.location) && put_calls == 1 &&
                      no_other_actions(),
                      "unsaved Reader B cannot replace authoritative persisted A");
            }
        }
    }
    cn_reader_position_clear(&captured);
    cn_reader_position_clear(&after);
    cn_ui_free(ui); cn_library_free(library);
    cn_progress_store_close(store);
    fake = 0;
    printf("LOCAL PUSH SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int parse_number(const char *text, unsigned long max,
                        unsigned long *value)
{
    char *end;
    unsigned long parsed;
    if (!text || !text[0] || *text == '-') return 0;
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
    event.y = CN_UI_LIB_ROW_TOP + 10; (void)cn_ui_handle(ui, &event);
    if (cn_ui_get_state(ui) != CN_UI_READER) {
        cn_ui_free(ui); cn_library_free(library); *lib = NULL; return NULL;
    }
    return ui;
}
static void go_pages(cn_ui *ui, int count, cn_input_event type)
{
    cn_input_ev event;
    memset(&event, 0, sizeof event); event.type = type;
    while (count-- > 0) (void)cn_ui_handle(ui, &event);
}
static int ui_matches(cn_ui *ui, const char *location)
{
    cn_reader_position position;
    int matches;
    cn_reader_position_init(&position);
    matches = cn_ui_reader_get_position(ui, &position) == 0 &&
              location && position.location &&
              !strcmp(location, position.location);
    cn_reader_position_clear(&position);
    return matches;
}

/* Synthetic setup and remote-verification requests are separate processes.
 * Only the push phase's own transcript delta proves GET=0/PUT=1. */
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
    cn_sync_push_result result;
    cn_dns_config dns;
    cn_time_config time;
    cn_tls_config tls;
    cn_kosync_client client, routed;
    cn_kosync_progress setup, remote;
    cn_kosync_outcome outcome;
    cn_dns_answer answer;
    cn_time_sample sample;
    cn_koreader_document_id remote_id;
    cn_book_identity local_id;
    cn_progress_record persisted, after, seed_record;
    cn_reader_position a, b;
    cn_ui *ui = NULL;
    cn_library *library = NULL;
    struct stat root_st;
    char gate[CN_STORAGE_PATH_CAPACITY], config_dir[CN_STORAGE_PATH_CAPACITY];
    char state_dir[CN_STORAGE_PATH_CAPACITY], progress_dir[CN_STORAGE_PATH_CAPACITY];
    unsigned long major_number, minor_number, dns_port, time_port;
    const char *phase, *scenario, *account, *document_id;
    int rc = 1, saved_stderr = -1, sink = -1, local_equal = 0;
    int reader_equal = 0, remote_equal = 0;

    if (argc != 15) return 2;
    phase = argv[2]; scenario = argv[3];
    if (strcmp(phase, "seed-local") && strcmp(phase, "seed-remote") &&
        strcmp(phase, "seed-unsupported") && strcmp(phase, "push") &&
        strcmp(phase, "verify-remote")) return 2;
    if (strcmp(scenario, "present") && strcmp(scenario, "equal") &&
        strcmp(scenario, "missing") && strcmp(scenario, "missing-local") &&
        strcmp(scenario, "unsupported")) return 2;
    if ((!strcmp(phase, "seed-remote") && strcmp(scenario, "present")) ||
        (!strcmp(phase, "seed-unsupported") && strcmp(scenario, "unsupported")) ||
        (!strcmp(phase, "seed-local") && strcmp(scenario, "present") &&
         strcmp(scenario, "missing"))) return 2;
    account = !strcmp(scenario, "present") || !strcmp(scenario, "equal")
                  ? "integration-local-only" : "integration-both-missing";
    if (!parse_number(argv[6], 0xffffffffUL, &major_number) ||
        !parse_number(argv[7], 0xffffffffUL, &minor_number) ||
        !parse_number(argv[11], 65535, &dns_port) || !dns_port ||
        !parse_number(argv[13], 65535, &time_port) || !time_port) return 2;
    candidate.mountpoint = argv[4]; candidate.root = argv[5];
    candidate.expected_major = (unsigned)major_number;
    candidate.expected_minor = (unsigned)minor_number;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) != CN_PLATFORM_STORAGE_OK) {
        puts("LOCAL PUSH GATE storage=unverified persistence=not-attempted network=not-attempted");
        return 1;
    }
    if (stat(verified.root, &root_st) != 0 ||
        snprintf(gate, sizeof gate, "%s/controller-%s", verified.root,
                 !strcmp(account, "integration-local-only") ? "upload" : "no-state") >=
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

    cn_progress_record_init(&persisted); cn_progress_record_init(&after);
    cn_progress_record_init(&seed_record);
    cn_reader_position_init(&a); cn_reader_position_init(&b);
    cn_kosync_progress_init(&setup); cn_kosync_progress_init(&remote);
    memset(&credentials, 0, sizeof credentials);
    memset(&client, 0, sizeof client); memset(&routed, 0, sizeof routed);
    /* Reader emits a book path on stderr; status output must not include it. */
    saved_stderr = dup(STDERR_FILENO);
    sink = open("/dev/null", O_WRONLY);
    if (saved_stderr < 0 || sink < 0 || dup2(sink, STDERR_FILENO) < 0) goto done;
    close(sink); sink = -1;
    if (cn_settings_load(&settings_store, &settings, NULL) != CN_SETTINGS_OK ||
        !settings.kosync_enabled ||
        cn_credential_store_load(&credential_store, &credentials, NULL) != CN_CREDENTIAL_OK ||
        strcmp(credentials.username, account) ||
        strcmp(credentials.userkey, MOCK_KEY)) goto done;
    cn_credentials_clear(&credentials);
    ui = open_reader(argv[8], argv[9], &library);
    if (!ui || cn_book_identity_from_path(&local_id, argv[9]) != 0) goto done;
    go_pages(ui, 5, CN_INPUT_PAGE_NEXT);
    if (cn_ui_reader_get_position(ui, &a) != 0 || !a.location) goto done;
    if (!strcmp(phase, "seed-local") || !strcmp(phase, "seed-unsupported")) {
        seed_record.position = a;
        if (!strcmp(phase, "seed-unsupported"))
            seed_record.position.progress_10000 = -1;
        rc = __real_cn_progress_store_save(store, &local_id, &seed_record) == CN_PROGRESS_OK
                 ? 0 : 1;
        seed_record.position.location = NULL;
        puts(rc ? "LOCAL PUSH GATE seed-local=failed" :
                  !strcmp(phase, "seed-unsupported")
                      ? "LOCAL PUSH GATE seed-unsupported=ok"
                      : "LOCAL PUSH GATE seed-local=ok");
        goto done;
    }
    if (cn_progress_store_load(store, &local_id, &persisted) != CN_PROGRESS_OK) {
        if (strcmp(scenario, "missing-local") || strcmp(phase, "push")) goto done;
    } else if (!persisted.position.location ||
               strcmp(persisted.position.location, a.location)) goto done;

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
    config.device_id = "synthetic-local-push-runtime-id";

    if (!strcmp(phase, "push")) {
        result = cn_sync_push_local_current_book(&config);
        local_equal = persisted.position.location &&
                      cn_progress_store_load(store, &local_id, &after) == CN_PROGRESS_OK &&
                      after.position.location &&
                      !strcmp(after.position.location, persisted.position.location) &&
                      after.position.progress_10000 == persisted.position.progress_10000;
        reader_equal = ui_matches(ui, a.location);
        printf("LOCAL PUSH GATE outcome=%s stage=%s put-invoked=%d "
               "local-load=%s local-mutation=%s remote-mutation=%s "
               "local-unchanged=%s reader-unchanged=%s\n",
               cn_sync_push_outcome_name(result.outcome),
               cn_sync_push_stage_name(result.stage), result.push.put_invoked,
               cn_progress_result_name(result.push.local_load_result),
               cn_kosync_mutation_state_name(result.local_mutation),
               cn_kosync_mutation_state_name(result.remote_mutation),
               local_equal ? "yes" : "no", reader_equal ? "yes" : "no");
        if (!strcmp(scenario, "missing-local"))
            rc = result.outcome == CN_SYNC_PUSH_LOCAL_MISSING_OUTCOME &&
                 !result.push.put_invoked && reader_equal ? 0 : 1;
        else if (!strcmp(scenario, "unsupported"))
            rc = result.outcome == CN_SYNC_PUSH_LOCAL_UNSUPPORTED_OUTCOME &&
                 !result.push.put_invoked && local_equal && reader_equal ? 0 : 1;
        else
            rc = result.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME &&
                 result.push.put_invoked && result.remote_mutation ==
                     CN_KOSYNC_MUTATION_CONFIRMED && local_equal && reader_equal ? 0 : 1;
        goto done;
    }

    if (cn_kosync_client_init(&client, settings.kosync_base_url,
                              account, MOCK_KEY) != CN_KOSYNC_OK ||
        !client.use_tls ||
        cn_book_identity_koreader_binary(argv[9], &remote_id) !=
            CN_KOREADER_IDENTITY_OK) goto done;
    document_id = cn_koreader_document_id_text(&remote_id);
    if (!document_id || cn_timesimple_sync(&time, &sample) != CN_TIME_OK ||
        cn_dnssimple_resolve_a(&dns, client.host, &answer) != CN_DNS_OK ||
        answer.count == 0) goto done;
    routed = client;
    if (cn_kosync_client_set_tls(&routed, &tls, answer.ipv4[0]) != CN_KOSYNC_OK)
        goto done;
    if (!strcmp(phase, "seed-remote")) {
        go_pages(ui, 4, CN_INPUT_PAGE_PREV);
        if (cn_ui_reader_get_position(ui, &b) != 0 || !b.location ||
            !strcmp(b.location, a.location) ||
            cn_kosync_progress_set(&setup, document_id, b.location,
                                    b.progress_10000, "synthetic-setup-device",
                                    "synthetic-setup-id") != CN_KOSYNC_OK ||
            cn_kosync_put_progress(&routed, &setup, NULL, &outcome) != CN_KOSYNC_OK)
            goto done;
        puts("LOCAL PUSH GATE seed-remote=ok setup-put=1");
        rc = 0;
        goto done;
    }
    if (strcmp(phase, "verify-remote")) goto done;
    if (cn_kosync_get_progress(&routed, document_id, &remote, &outcome) ==
        CN_KOSYNC_OK && persisted.position.location && remote.logical_position)
        remote_equal = !strcmp(remote.logical_position, persisted.position.location) &&
                       remote.progress_10000 == persisted.position.progress_10000;
    reader_equal = ui_matches(ui, a.location);
    printf("LOCAL PUSH GATE verify-remote=%s reader-unchanged=%s network=GET-only\n",
           remote_equal ? "ok" : "failed", reader_equal ? "yes" : "no");
    rc = remote_equal && reader_equal ? 0 : 1;
done:
    if (saved_stderr >= 0) {
        fflush(stderr);
        (void)dup2(saved_stderr, STDERR_FILENO);
        close(saved_stderr);
    }
    if (sink >= 0) close(sink);
    cn_kosync_progress_clear(&setup); cn_kosync_progress_clear(&remote);
    cn_credentials_clear(&credentials);
    memset(&client, 0, sizeof client); memset(&routed, 0, sizeof routed);
    cn_reader_position_clear(&a); cn_reader_position_clear(&b);
    cn_progress_record_clear(&persisted); cn_progress_record_clear(&after);
    cn_progress_record_clear(&seed_record);
    cn_ui_free(ui); cn_library_free(library);
    cn_progress_store_close(store);
    if (rc != 0) puts("LOCAL PUSH GATE result=failed");
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--smoke"))
        return smoke(argv[2], argv[3], argv[4]);
    if (argc >= 2 && !strcmp(argv[1], "--physical"))
        return physical(argc, argv);
    fputs("usage: local-push-test --smoke <font> <epub> <existing-directory> | --physical <seed-local|seed-remote|seed-unsupported|push|verify-remote> <present|equal|missing|missing-local|unsupported> <mount> <root> <major> <minor> <font> <epub> <dns-ip> <dns-port> <sntp-ip> <sntp-port> <ca>\n", stderr);
    return 2;
}
