/* Synthetic orchestration matrix and later verified-card one-shot gate. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/storage_verify.h"
#include "storage/storage_layout.h"
#include "sync/sync_controller.h"

#define TEST_USER "crossnook-synthetic-controller-account"
#define TEST_KEY "crossnook-synthetic-controller-key"
#define TEST_ID "crossnook-synthetic-device-id"
#define TEST_POSITION "/body/DocFragment[1]/body/p[1]/text().0"
/* Existing controlled mock fixture key; never log it or pass via argv. */
#define MOCK_KEY "dfb450efddbb5387197c84460623675b"

static const char *scenario_user(const char *scenario)
{
    if (!strcmp(scenario, "no-state")) return "integration-both-missing";
    if (!strcmp(scenario, "no-change")) return "integration-same-percentage-different";
    if (!strcmp(scenario, "upload")) return "integration-local-only";
    if (!strcmp(scenario, "conflict")) return "integration-different";
    return NULL;
}

static int fake_enabled;
static cn_settings_result settings_mode;
static cn_credential_result credential_mode;
static int settings_enabled, invalid_url, classifier_failure;
static int insecure_url;
static int settings_calls, credential_calls, integration_calls, policy_calls;
static int physical_credential_loads, physical_integration_calls, physical_policy_calls;
static int clear_calls, cleared_value;
static int failures;
static char call_order[8];
static unsigned call_order_length;

static void record_call(char operation)
{
    if (call_order_length + 1 < sizeof call_order) {
        call_order[call_order_length++] = operation;
        call_order[call_order_length] = '\0';
    } else ++failures;
}

typedef enum case_kind {
    NO_STATE, UPLOAD, IMPORT, NO_CHANGE, CONFLICT,
    GET_AUTH, GET_CONNECT, BAD_TIME, PUT_AUTH, PUT_UNCERTAIN, SAVE_FAILED
} case_kind;
static case_kind selected;

cn_settings_result __real_cn_settings_load(const cn_settings_store *, cn_settings *, int *);
cn_credential_result __real_cn_credential_store_load(const cn_credential_store *, cn_credentials *, int *);
cn_kosync_sync_status __real_cn_kosync_sync_once(const cn_kosync_sync_config *, cn_kosync_sync_result *);
int __real_cn_kosync_policy_classify(int, const cn_kosync_sync_result *, cn_kosync_product_result *);
void __real_cn_credentials_clear(cn_credentials *);

cn_settings_result __wrap_cn_settings_load(const cn_settings_store *store,
                                           cn_settings *out, int *error)
{
    if (!fake_enabled) return __real_cn_settings_load(store, out, error);
    ++settings_calls;
    record_call('S');
    cn_settings_defaults(out);
    if (settings_mode == CN_SETTINGS_OK) {
        out->kosync_enabled = settings_enabled;
        strcpy(out->kosync_device_name, "Synthetic reader");
        if (insecure_url)
            strcpy(out->kosync_base_url, "http://sync.synthetic.invalid/");
        else if (!invalid_url)
            strcpy(out->kosync_base_url, "https://sync.synthetic.invalid/");
    }
    return settings_mode;
}
cn_credential_result __wrap_cn_credential_store_load(
    const cn_credential_store *store, cn_credentials *out, int *error)
{
    if (!fake_enabled) {
        ++physical_credential_loads;
        return __real_cn_credential_store_load(store, out, error);
    }
    ++credential_calls;
    record_call('C');
    (void)store; (void)error;
    strcpy(out->username, TEST_USER);
    strcpy(out->userkey, TEST_KEY);
    return credential_mode;
}
void __wrap_cn_credentials_clear(cn_credentials *value)
{
    if (fake_enabled) {
        ++clear_calls;
        if (value && strcmp(value->username, TEST_USER) == 0 &&
            strcmp(value->userkey, TEST_KEY) == 0)
            ++cleared_value;
    }
    __real_cn_credentials_clear(value);
}
cn_kosync_sync_status __wrap_cn_kosync_sync_once(
    const cn_kosync_sync_config *config, cn_kosync_sync_result *out)
{
    if (!fake_enabled) {
        ++physical_integration_calls;
        return __real_cn_kosync_sync_once(config, out);
    }
    ++integration_calls;
    record_call('I');
    if (!config || !config->client || !config->client->use_tls ||
        strcmp(config->client->username, TEST_USER) ||
        strcmp(config->client->userkey, TEST_KEY) ||
        strcmp(config->device, "Synthetic reader") ||
        strcmp(config->device_id, TEST_ID) ||
        !config->document_path || !config->progress_store ||
        !config->dns || !config->tls || !config->time) {
        ++failures;
        return CN_KOSYNC_SYNC_STATUS_INVALID;
    }
    out->status = CN_KOSYNC_SYNC_STATUS_OK;
    out->local_load_result = CN_PROGRESS_MISSING;
    out->kosync_result = CN_KOSYNC_NOT_FOUND;
    out->time_result = CN_TIME_OK;
    out->dns_result = CN_DNS_OK;
    switch (selected) {
    case NO_STATE: out->decision = CN_KOSYNC_SYNC_NO_STATE; break;
    case UPLOAD:
        out->decision = CN_KOSYNC_SYNC_LOCAL_SELECTED;
        out->local_present = 1; out->local_load_result = CN_PROGRESS_OK;
        out->remote_put_attempted = 1; out->remote_uploaded = 1;
        out->kosync_result = CN_KOSYNC_OK;
        break;
    case IMPORT:
        out->decision = CN_KOSYNC_SYNC_REMOTE_SELECTED;
        out->remote_present = 1; out->local_save_attempted = 1;
        out->local_saved = 1; out->local_save_result = CN_PROGRESS_OK;
        out->kosync_result = CN_KOSYNC_OK;
        break;
    case NO_CHANGE: case CONFLICT:
        out->decision = selected == NO_CHANGE ? CN_KOSYNC_SYNC_NO_CHANGE : CN_KOSYNC_SYNC_AMBIGUOUS;
        out->local_present = out->remote_present = 1;
        out->local_load_result = CN_PROGRESS_OK;
        out->kosync_result = CN_KOSYNC_OK;
        break;
    case GET_AUTH:
        out->status = CN_KOSYNC_SYNC_STATUS_AUTH_FAILED;
        out->kosync_result = CN_KOSYNC_AUTH_FAILED;
        break;
    case GET_CONNECT:
        out->status = CN_KOSYNC_SYNC_STATUS_DNS_FAILED;
        out->dns_result = CN_DNS_TIMEOUT;
        break;
    case BAD_TIME:
        out->status = CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED;
        out->time_result = CN_TIME_TIMEOUT;
        break;
    case PUT_AUTH: case PUT_UNCERTAIN:
        out->local_present = 1; out->local_load_result = CN_PROGRESS_OK;
        out->remote_put_attempted = 1;
        out->status = selected == PUT_AUTH ? CN_KOSYNC_SYNC_STATUS_AUTH_FAILED
                                           : CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED;
        out->kosync_result = selected == PUT_AUTH ? CN_KOSYNC_AUTH_FAILED
                                                  : CN_KOSYNC_TRANSPORT_ERROR;
        out->kosync_outcome.transport_result = CN_NETSIMPLE_SEND_TIMEOUT;
        break;
    case SAVE_FAILED:
        out->remote_present = 1; out->local_save_attempted = 1;
        out->local_save_result = CN_PROGRESS_IO_ERROR;
        out->status = CN_KOSYNC_SYNC_STATUS_LOCAL_FAILED;
        out->kosync_result = CN_KOSYNC_OK;
        break;
    }
    return out->status;
}
int __wrap_cn_kosync_policy_classify(int enabled,
                                      const cn_kosync_sync_result *sync,
                                      cn_kosync_product_result *out)
{
    if (!fake_enabled) ++physical_policy_calls;
    if (fake_enabled) ++policy_calls;
    if (fake_enabled) record_call('P');
    if (fake_enabled && classifier_failure) return -1;
    return __real_cn_kosync_policy_classify(enabled, sync, out);
}

static void check(int valid, const char *name)
{
    printf("[%s] %s\n", valid ? "OK" : "FAIL", name);
    if (!valid) ++failures;
}
static void reset_fake(void)
{
    fake_enabled = 1;
    settings_mode = CN_SETTINGS_OK; credential_mode = CN_CREDENTIAL_OK;
    settings_enabled = 1; invalid_url = insecure_url = classifier_failure = 0;
    settings_calls = credential_calls = integration_calls = policy_calls = 0;
    clear_calls = cleared_value = 0;
    call_order_length = 0; call_order[0] = 0;
    selected = NO_STATE;
}

static cn_sync_controller_result invoke(const char *device_id)
{
    cn_sync_controller_config config;
    cn_dns_config dns = {{"127.0.0.2"}, 1, 53, 200};
    cn_time_config time = {{"127.0.0.2"}, 1, 123, 200};
    cn_tls_config tls = {"/tmp/synthetic-ca", NULL, NULL};
    memset(&config, 0, sizeof config);
    config.settings_store = (const cn_settings_store *)&config;
    config.credential_store = (const cn_credential_store *)&config;
    config.progress_store = (cn_progress_store *)&config;
    config.document_path = "/tmp/synthetic-book.epub";
    config.device_id = device_id;
    config.dns = &dns; config.time = &time; config.tls = &tls;
    config.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    return cn_sync_current_book(&config);
}

static void success_case(case_kind kind, cn_kosync_product_outcome outcome,
                         cn_kosync_retry_policy retry,
                         cn_kosync_mutation_state local,
                         cn_kosync_mutation_state remote,
                         const char *label)
{
    cn_sync_controller_result got;
    reset_fake(); selected = kind;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_INTEGRATION &&
          got.product.outcome == outcome && got.product.retry == retry &&
          got.product.local_mutation == local && got.product.remote_mutation == remote &&
          got.local_save_attempted == (kind == IMPORT || kind == SAVE_FAILED) &&
          got.remote_put_attempted ==
              (kind == UPLOAD || kind == PUT_AUTH || kind == PUT_UNCERTAIN) &&
          settings_calls == 1 && credential_calls == 1 &&
          integration_calls == 1 && policy_calls == 1 &&
          strcmp(call_order, "SCIP") == 0 &&
          clear_calls == 1 && cleared_value == 1, label);
}

static int smoke(void)
{
    cn_sync_controller_result got;
    char oversized_id[CN_KOSYNC_DEVICE_MAX + 2];
    reset_fake(); settings_mode = CN_SETTINGS_MISSING;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_DISABLED &&
          got.settings_result == CN_SETTINGS_MISSING &&
          got.product.outcome == CN_KOSYNC_PRODUCT_DISABLED &&
          got.product.retry == CN_KOSYNC_RETRY_NONE &&
          settings_calls == 1 && credential_calls == 0 &&
          integration_calls == 0 && policy_calls == 1 &&
          strcmp(call_order, "SP") == 0,
          "missing settings use disabled defaults, no credentials/network");
    reset_fake(); settings_enabled = 0;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_DISABLED &&
          credential_calls == 0 && integration_calls == 0 && policy_calls == 1,
          "explicit disabled short-circuits");
    reset_fake(); settings_mode = CN_SETTINGS_CORRUPT;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_SETTINGS_FAILED &&
          got.settings_result == CN_SETTINGS_CORRUPT &&
          got.product.outcome == CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE &&
          !credential_calls && !integration_calls && !policy_calls,
          "corrupt settings are not disabled defaults");
    reset_fake(); settings_mode = CN_SETTINGS_UNSUPPORTED_VERSION;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_SETTINGS_FAILED &&
          got.settings_result == CN_SETTINGS_UNSUPPORTED_VERSION && !credential_calls &&
          !integration_calls, "unsupported settings stop preflight");
    reset_fake(); settings_mode = CN_SETTINGS_IO_ERROR;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_SETTINGS_FAILED &&
          got.settings_result == CN_SETTINGS_IO_ERROR && !credential_calls &&
          !integration_calls, "settings filesystem failure is not disabled");
    reset_fake(); credential_mode = CN_CREDENTIAL_MISSING;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CREDENTIALS_FAILED &&
          got.product.outcome == CN_KOSYNC_PRODUCT_AUTH_REQUIRED &&
          got.credential_result == CN_CREDENTIAL_MISSING &&
          !integration_calls && !policy_calls && clear_calls == 1 && cleared_value == 1,
          "missing credentials require auth; secret scratch cleared");
    reset_fake(); credential_mode = CN_CREDENTIAL_CORRUPT;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CREDENTIALS_FAILED &&
          got.credential_result == CN_CREDENTIAL_CORRUPT &&
          got.product.outcome == CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE &&
          !integration_calls && cleared_value == 1,
          "corrupt credentials distinct from missing");
    reset_fake(); credential_mode = CN_CREDENTIAL_UNSUPPORTED_VERSION;
    got = invoke(TEST_ID);
    check(got.credential_result == CN_CREDENTIAL_UNSUPPORTED_VERSION &&
          got.stage == CN_SYNC_CONTROLLER_CREDENTIALS_FAILED &&
          !integration_calls, "unsupported credentials stop preflight");
    reset_fake(); credential_mode = CN_CREDENTIAL_IO_ERROR;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CREDENTIALS_FAILED &&
          got.credential_result == CN_CREDENTIAL_IO_ERROR &&
          got.product.outcome == CN_KOSYNC_PRODUCT_LOCAL_FAILURE &&
          !integration_calls, "credential I/O failure is not auth missing");
    reset_fake(); invalid_url = 1;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED && !integration_calls &&
          clear_calls == 1 && cleared_value == 1,
          "missing HTTPS URL rejected before network");
    reset_fake(); insecure_url = 1;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED && !integration_calls,
          "insecure URL rejected before network");
    reset_fake(); got = invoke(NULL);
    check(got.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED && !integration_calls,
          "missing device ID rejected without generating one");
    reset_fake(); got = invoke("bad\nidentity");
    check(got.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED && !integration_calls,
          "invalid device ID rejected before network");
    memset(oversized_id, 'x', sizeof oversized_id);
    oversized_id[sizeof oversized_id - 1] = 0;
    reset_fake(); got = invoke(oversized_id);
    check(got.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED && !integration_calls,
          "oversized device ID rejected before network");
    success_case(NO_STATE, CN_KOSYNC_PRODUCT_NO_STATE, CN_KOSYNC_RETRY_NONE,
                 CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE, "no state no mutation");
    success_case(UPLOAD, CN_KOSYNC_PRODUCT_UPLOADED, CN_KOSYNC_RETRY_NONE,
                 CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_CONFIRMED,
                 "local only uploads via one integration call");
    success_case(IMPORT, CN_KOSYNC_PRODUCT_IMPORTED, CN_KOSYNC_RETRY_NONE,
                 CN_KOSYNC_MUTATION_CONFIRMED, CN_KOSYNC_MUTATION_NONE,
                 "remote only imports via one integration call");
    success_case(NO_CHANGE, CN_KOSYNC_PRODUCT_UNCHANGED, CN_KOSYNC_RETRY_NONE,
                 CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                 "equal canonical position has no mutation even with percentage difference");
    success_case(CONFLICT, CN_KOSYNC_PRODUCT_CONFLICT, CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                 CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                 "different canonical positions conflict with no mutation");
    success_case(GET_AUTH, CN_KOSYNC_PRODUCT_AUTH_REQUIRED, CN_KOSYNC_RETRY_EXPLICIT_ACTION,
                 CN_KOSYNC_MUTATION_NONE, CN_KOSYNC_MUTATION_NONE,
                 "GET auth rejection requires explicit action");
    success_case(GET_CONNECT, CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE,
                 CN_KOSYNC_RETRY_AUTOMATIC_LATER, CN_KOSYNC_MUTATION_NONE,
                 CN_KOSYNC_MUTATION_NONE, "GET DNS failure propagated");
    success_case(BAD_TIME, CN_KOSYNC_PRODUCT_TRUSTED_TIME_UNAVAILABLE,
                 CN_KOSYNC_RETRY_AUTOMATIC_LATER, CN_KOSYNC_MUTATION_NONE,
                 CN_KOSYNC_MUTATION_NONE, "trusted time unavailable distinct");
    success_case(PUT_AUTH, CN_KOSYNC_PRODUCT_AUTH_REQUIRED,
                 CN_KOSYNC_RETRY_EXPLICIT_ACTION, CN_KOSYNC_MUTATION_NONE,
                 CN_KOSYNC_MUTATION_NONE, "PUT authentication failure proves no mutation");
    success_case(PUT_UNCERTAIN, CN_KOSYNC_PRODUCT_CONNECTIVITY_FAILURE,
                 CN_KOSYNC_RETRY_AUTOMATIC_LATER, CN_KOSYNC_MUTATION_NONE,
                 CN_KOSYNC_MUTATION_POSSIBLE, "uncertain PUT has no blind retry");
    success_case(SAVE_FAILED, CN_KOSYNC_PRODUCT_LOCAL_FAILURE,
                 CN_KOSYNC_RETRY_EXPLICIT_ACTION, CN_KOSYNC_MUTATION_POSSIBLE,
                 CN_KOSYNC_MUTATION_NONE, "failed local import conservatively possible");
    reset_fake(); selected = PUT_UNCERTAIN; classifier_failure = 1;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CLASSIFICATION_FAILED &&
          got.product.outcome == CN_KOSYNC_PRODUCT_INTERNAL_FAILURE &&
          got.product.retry == CN_KOSYNC_RETRY_EXPLICIT_ACTION &&
          got.remote_put_attempted == 1 &&
          got.product.remote_mutation == CN_KOSYNC_MUTATION_POSSIBLE &&
          got.mutation_evidence_conservative && integration_calls == 1 && policy_calls == 1,
          "classifier failure retains conservative possible remote mutation");
    reset_fake(); selected = SAVE_FAILED; classifier_failure = 1;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CLASSIFICATION_FAILED &&
          got.local_save_attempted &&
          got.product.local_mutation == CN_KOSYNC_MUTATION_POSSIBLE &&
          !got.remote_put_attempted,
          "classifier failure retains possible local mutation");
    reset_fake(); selected = UPLOAD; classifier_failure = 1;
    got = invoke(TEST_ID);
    check(got.stage == CN_SYNC_CONTROLLER_CLASSIFICATION_FAILED &&
          got.remote_put_attempted && got.remote_uploaded &&
          got.product.remote_mutation == CN_KOSYNC_MUTATION_CONFIRMED,
          "classifier failure retains confirmed remote mutation");
    reset_fake(); selected = UPLOAD;
    got = invoke(TEST_ID);
    check(got.remote_uploaded && !got.local_save_attempted && !got.local_saved &&
          cn_sync_controller_stage_name(CN_SYNC_CONTROLLER_INTEGRATION) != NULL &&
          cn_sync_controller_stage_name((cn_sync_controller_stage)999) != NULL,
          "confirmed PUT has no subsequent local-save phase");
    fake_enabled = 0;
    printf("SYNC CONTROLLER SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

/* Physical phases always verify the actual external mount before any storage I/O. */
static int physical(int argc, char **argv)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_progress_store *progress_store = NULL;
    cn_sync_controller_config config;
    cn_sync_controller_result result;
    cn_dns_config dns;
    cn_time_config time;
    cn_tls_config tls;
    cn_settings settings;
    cn_credentials credentials;
    cn_book_identity identity;
    cn_progress_record record;
    cn_settings_result settings_status;
    cn_credential_result credential_status;
    char gate_root[CN_STORAGE_PATH_CAPACITY], config_dir[CN_STORAGE_PATH_CAPACITY];
    char progress_dir[CN_STORAGE_PATH_CAPACITY], state_dir[CN_STORAGE_PATH_CAPACITY];
    char *end;
    unsigned long major_number, minor_number, dns_port, time_port;
    struct stat root_st, gate_st;
    int rc = 1;
    const char *action, *scenario;
    const char *mock_user;

    if (argc != 15) return 2;
    action = argv[2]; scenario = argv[3];
    if (strcmp(action, "prepare") && strcmp(action, "seed") && strcmp(action, "run")) return 2;
    if (strcmp(scenario, "disabled") && strcmp(scenario, "no-state") &&
        strcmp(scenario, "no-change") && strcmp(scenario, "upload") &&
        strcmp(scenario, "conflict")) return 2;
    mock_user = scenario_user(scenario);
    errno = 0; major_number = strtoul(argv[6], &end, 10);
    if (errno || !argv[6][0] || *end || major_number > 0xffffffffUL) return 2;
    errno = 0; minor_number = strtoul(argv[7], &end, 10);
    if (errno || !argv[7][0] || *end || minor_number > 0xffffffffUL) return 2;
    errno = 0; dns_port = strtoul(argv[11], &end, 10);
    if (errno || !argv[11][0] || *end || dns_port == 0 || dns_port > 65535) return 2;
    errno = 0; time_port = strtoul(argv[13], &end, 10);
    if (errno || !argv[13][0] || *end || time_port == 0 || time_port > 65535) return 2;
    candidate.mountpoint = argv[4]; candidate.root = argv[5];
    candidate.expected_major = (unsigned)major_number;
    candidate.expected_minor = (unsigned)minor_number;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) != CN_PLATFORM_STORAGE_OK) {
        puts("SYNC CONTROLLER GATE storage=unverified persistence=not-attempted");
        return 1;
    }
    if (snprintf(gate_root, sizeof gate_root, "%s/controller-%s", verified.root, scenario) >=
        (int)sizeof gate_root) return 1;
    if (!strcmp(action, "prepare") && mkdir(gate_root, 0700) != 0) return 1;
    if (stat(verified.root, &root_st) != 0 || lstat(gate_root, &gate_st) != 0 ||
        !S_ISDIR(gate_st.st_mode) || root_st.st_dev != gate_st.st_dev) return 1;
    if (cn_storage_layout_init(&layout, gate_root, NULL) != CN_STORAGE_OK ||
        (!strcmp(action, "prepare") && cn_storage_layout_prepare(&layout, NULL) != CN_STORAGE_OK) ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG,
                               config_dir, sizeof config_dir) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                               progress_dir, sizeof progress_dir) != CN_STORAGE_OK ||
        snprintf(state_dir, sizeof state_dir, "%s/state", gate_root) >= (int)sizeof state_dir ||
        cn_settings_store_init(&settings_store, config_dir, NULL) != CN_SETTINGS_OK ||
        cn_credential_store_init(&credential_store, state_dir, NULL) != CN_CREDENTIAL_OK ||
        cn_progress_store_open(&progress_store, progress_dir) != CN_PROGRESS_OK) return 1;
    settings_status = cn_settings_load(&settings_store, &settings, NULL);
    memset(&credentials, 0, sizeof credentials);
    credential_status = !strcmp(action, "run") && !strcmp(scenario, "disabled")
                            ? CN_CREDENTIAL_MISSING
                            : cn_credential_store_load(&credential_store,
                                                       &credentials, NULL);
    if (!strcmp(action, "prepare")) {
        if (settings_status != CN_SETTINGS_MISSING ||
            credential_status != CN_CREDENTIAL_MISSING) goto done;
        if (strcmp(scenario, "disabled")) {
            cn_settings_defaults(&settings);
            settings.kosync_enabled = 1;
            if (strlen(argv[9]) >= sizeof settings.kosync_base_url) goto done;
            strcpy(settings.kosync_base_url, argv[9]);
            strcpy(settings.kosync_device_name, "Synthetic reader");
            if (cn_settings_validate(&settings) != CN_SETTINGS_OK) goto done;
            memset(&credentials, 0, sizeof credentials);
            strcpy(credentials.username, mock_user);
            strcpy(credentials.userkey, MOCK_KEY);
            if (cn_settings_save(&settings_store, &settings, NULL) != CN_SETTINGS_OK ||
                cn_credential_store_save(&credential_store, &credentials, NULL) != CN_CREDENTIAL_OK)
                goto done;
        }
        puts("SYNC CONTROLLER GATE prepare=ok"); rc = 0; goto done;
    }
    if (strcmp(scenario, "disabled") &&
        (settings_status != CN_SETTINGS_OK || credential_status != CN_CREDENTIAL_OK ||
         strcmp(credentials.username, mock_user) || strcmp(credentials.userkey, MOCK_KEY)))
        goto done;
    if (!strcmp(action, "run") && !strcmp(scenario, "disabled") &&
        settings_status != CN_SETTINGS_MISSING) goto done;
    if (!strcmp(action, "seed")) {
        if (!strcmp(scenario, "disabled") || !strcmp(scenario, "no-state") ||
            cn_book_identity_from_path(&identity, argv[8]) != 0) goto done;
        cn_progress_record_init(&record);
        record.position.location = (char *)TEST_POSITION;
        record.position.progress_10000 = 3210;
        if (cn_progress_store_save(progress_store, &identity, &record) != CN_PROGRESS_OK)
            goto done;
        record.position.location = NULL;
        puts("SYNC CONTROLLER GATE seed=ok"); rc = 0; goto done;
    }
    memset(&dns, 0, sizeof dns);
    dns.servers[0] = argv[10]; dns.server_count = 1; dns.port = (unsigned)dns_port;
    memset(&time, 0, sizeof time);
    time.servers[0] = argv[12]; time.server_count = 1; time.port = (unsigned)time_port;
    memset(&tls, 0, sizeof tls); tls.ca_path = argv[14];
    memset(&config, 0, sizeof config);
    config.settings_store = &settings_store;
    config.credential_store = &credential_store;
    config.progress_store = progress_store;
    config.document_path = argv[8]; config.device_id = TEST_ID;
    config.dns = &dns; config.tls = &tls;
    config.time = &time; config.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    physical_credential_loads = physical_integration_calls = physical_policy_calls = 0;
    result = cn_sync_current_book(&config);
    printf("SYNC CONTROLLER GATE stage=%s outcome=%s retry=%s local-mutation=%s "
           "remote-mutation=%s local-save-attempted=%d remote-put-attempted=%d "
           "credential-loads=%d sync-calls=%d policy-calls=%d\n",
           cn_sync_controller_stage_name(result.stage),
           cn_kosync_product_outcome_name(result.product.outcome),
           cn_kosync_retry_policy_name(result.product.retry),
           cn_kosync_mutation_state_name(result.product.local_mutation),
           cn_kosync_mutation_state_name(result.product.remote_mutation),
           result.local_save_attempted, result.remote_put_attempted,
           physical_credential_loads, physical_integration_calls,
           physical_policy_calls);
    rc = result.stage == CN_SYNC_CONTROLLER_DISABLED ||
         result.stage == CN_SYNC_CONTROLLER_INTEGRATION ? 0 : 1;
done:
    cn_credentials_clear(&credentials);
    cn_progress_store_close(progress_store);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--smoke")) return smoke();
    if (argc >= 2 && !strcmp(argv[1], "--physical")) return physical(argc, argv);
    fputs("usage: sync-controller-test --smoke | --physical <prepare|seed|run> <scenario> <mount> <root> <major> <minor> <book> <https-url> <dns-ip> <dns-port> <sntp-ip> <sntp-port> <ca>\n", stderr);
    return 2;
}
