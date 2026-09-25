#include <stdio.h>
#include <string.h>

#include "sync/profile_sync_controller.h"

#define TEST_BASE_URL "https://profile.synthetic.invalid/kosync"
#define TEST_USER "synthetic-profile-user"
#define TEST_KEY "synthetic-profile-userkey"
#define TEST_DEVICE "Synthetic profile reader"
#define TEST_DEVICE_ID "0123456789abcdef0123456789abcdef"

static int failures;
static int settings_loads;
static int credential_loads;
static int client_init_calls;
static int sync_calls;
static int push_calls;
static int pull_calls;
static int sync_inputs_ok;
static int push_inputs_ok;
static int pull_inputs_ok;
static const cn_dns_config *expected_dns;
static const cn_tls_config *expected_tls;
static const cn_time_config *expected_time;
static cn_progress_store *expected_progress;
static const char *expected_document;
static char client_base_url[CN_SETTINGS_KOSYNC_URL_CAPACITY];

cn_kosync_result __real_cn_kosync_client_init(
    cn_kosync_client *client, const char *base_url,
    const char *username, const char *userkey);

static void check(int valid, const char *name)
{
    printf("[%s] %s\n", valid ? "OK" : "FAIL", name);
    if (!valid) ++failures;
}

static void reset_counts(void)
{
    settings_loads = 0;
    credential_loads = 0;
    client_init_calls = 0;
    sync_calls = 0;
    push_calls = 0;
    pull_calls = 0;
    sync_inputs_ok = 0;
    push_inputs_ok = 0;
    pull_inputs_ok = 0;
    memset(client_base_url, 0, sizeof client_base_url);
}

cn_settings_result __wrap_cn_settings_load(const cn_settings_store *store,
                                           cn_settings *out, int *error)
{
    (void)store;
    (void)error;
    ++settings_loads;
    cn_settings_defaults(out);
    out->kosync_enabled = 1;
    strcpy(out->kosync_base_url, TEST_BASE_URL);
    strcpy(out->kosync_device_name, TEST_DEVICE);
    return CN_SETTINGS_OK;
}

cn_credential_result __wrap_cn_credential_store_load(
    const cn_credential_store *store, cn_credentials *out, int *error)
{
    (void)store;
    (void)error;
    ++credential_loads;
    strcpy(out->username, TEST_USER);
    strcpy(out->userkey, TEST_KEY);
    return CN_CREDENTIAL_OK;
}

void __wrap_cn_credentials_clear(cn_credentials *credentials)
{
    if (credentials) memset(credentials, 0, sizeof *credentials);
}

cn_kosync_result __wrap_cn_kosync_client_init(
    cn_kosync_client *client, const char *base_url,
    const char *username, const char *userkey)
{
    cn_kosync_result result;
    ++client_init_calls;
    if (!client || !base_url || !username || !userkey) return CN_KOSYNC_INVALID;
    result = __real_cn_kosync_client_init(client, base_url, username, userkey);
    if (result != CN_KOSYNC_OK) return result;
    if (strlen(base_url) >= sizeof client_base_url) return CN_KOSYNC_INVALID;
    strcpy(client_base_url, base_url);
    return result;
}

cn_kosync_sync_status __wrap_cn_kosync_sync_once(
    const cn_kosync_sync_config *config, cn_kosync_sync_result *out)
{
    ++sync_calls;
    sync_inputs_ok = config && config->client && config->client->use_tls &&
                     !strcmp(config->client->username, TEST_USER) &&
                     !strcmp(config->client->userkey, TEST_KEY) &&
                     !strcmp(config->device, TEST_DEVICE) &&
                     !strcmp(config->device_id, TEST_DEVICE_ID) &&
                     config->dns == expected_dns && config->tls == expected_tls &&
                     config->time == expected_time &&
                     config->progress_store == expected_progress &&
                     config->document_path == expected_document &&
                     config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH;
    if (!out) return CN_KOSYNC_SYNC_STATUS_INVALID;
    memset(out, 0, sizeof *out);
    out->status = sync_inputs_ok ? CN_KOSYNC_SYNC_STATUS_OK
                                 : CN_KOSYNC_SYNC_STATUS_INVALID;
    out->decision = CN_KOSYNC_SYNC_NO_STATE;
    out->local_load_result = CN_PROGRESS_MISSING;
    out->time_result = CN_TIME_OK;
    out->dns_result = CN_DNS_OK;
    out->kosync_result = CN_KOSYNC_NOT_FOUND;
    out->kosync_outcome.transport_result = CN_NETSIMPLE_INVALID;
    return out->status;
}

cn_kosync_push_status __wrap_cn_kosync_push_local_once(
    const cn_kosync_push_config *config, cn_kosync_push_result *out)
{
    ++push_calls;
    push_inputs_ok = config && config->client && config->client->use_tls &&
                     !strcmp(config->client->username, TEST_USER) &&
                     !strcmp(config->client->userkey, TEST_KEY) &&
                     !strcmp(config->device, TEST_DEVICE) &&
                     !strcmp(config->device_id, TEST_DEVICE_ID) &&
                     config->dns == expected_dns && config->tls == expected_tls &&
                     config->time == expected_time &&
                     config->progress_store == expected_progress &&
                     config->document_path == expected_document &&
                     config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH;
    if (!out) return CN_KOSYNC_PUSH_INVALID;
    memset(out, 0, sizeof *out);
    out->status = push_inputs_ok ? CN_KOSYNC_PUSH_UPLOADED
                                 : CN_KOSYNC_PUSH_INVALID;
    out->time_result = CN_TIME_OK;
    out->dns_result = CN_DNS_OK;
    out->kosync_result = CN_KOSYNC_OK;
    out->remote_uploaded = push_inputs_ok;
    out->remote_mutation = push_inputs_ok ? CN_KOSYNC_MUTATION_CONFIRMED
                                          : CN_KOSYNC_MUTATION_NONE;
    return out->status;
}

cn_kosync_pull_status __wrap_cn_kosync_pull_remote_once(
    const cn_kosync_pull_config *config, cn_kosync_pull_result *out)
{
    ++pull_calls;
    pull_inputs_ok = config && config->client && config->client->use_tls &&
                     !strcmp(config->client->username, TEST_USER) &&
                     !strcmp(config->client->userkey, TEST_KEY) &&
                     config->dns == expected_dns && config->tls == expected_tls &&
                     config->time == expected_time &&
                     config->progress_store == expected_progress &&
                     config->document_path == expected_document &&
                     config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH;
    if (!out) return CN_KOSYNC_PULL_INVALID;
    memset(out, 0, sizeof *out);
    out->status = pull_inputs_ok ? CN_KOSYNC_PULL_PERSISTED
                                 : CN_KOSYNC_PULL_INVALID;
    out->time_result = CN_TIME_OK;
    out->dns_result = CN_DNS_OK;
    out->kosync_result = CN_KOSYNC_OK;
    out->local_save_attempted = pull_inputs_ok;
    out->local_saved = pull_inputs_ok;
    return out->status;
}

static cn_persisted_sync_profile profile(void)
{
    cn_persisted_sync_profile value;
    memset(&value, 0, sizeof value);
    strcpy(value.base_url, TEST_BASE_URL);
    strcpy(value.username, TEST_USER);
    strcpy(value.userkey, TEST_KEY);
    strcpy(value.device_id, TEST_DEVICE_ID);
    strcpy(value.device_name, TEST_DEVICE);
    return value;
}

static int smoke(void)
{
    cn_persisted_sync_profile value = profile();
    cn_dns_config dns = {{"profile.synthetic.invalid"}, 1, 53, 200};
    cn_tls_config tls = {"/tmp/profile-synthetic-ca", NULL, NULL};
    cn_time_config time = {{"time.synthetic.invalid"}, 1, 123, 200};
    cn_sync_runtime_inputs runtime = {
        &dns, &tls, CN_KOSYNC_SYNC_TIME_ESTABLISH, &time
    };
    cn_progress_store *progress = (cn_progress_store *)&value;
    const char *document = "/tmp/profile-synthetic-book.epub";
    cn_sync_controller_result normal;
    cn_sync_push_result push;
    cn_sync_pull_result pull;
    cn_sync_controller_result invalid;

    expected_dns = &dns;
    expected_tls = &tls;
    expected_time = &time;
    expected_progress = progress;
    expected_document = document;
    reset_counts();

    normal = cn_sync_current_book_with_profile(&value, &runtime, progress,
                                               document);
    check(normal.stage == CN_SYNC_CONTROLLER_INTEGRATION &&
          normal.product.outcome == CN_KOSYNC_PRODUCT_NO_STATE &&
          normal.settings_result == CN_SETTINGS_INVALID_ARGUMENT &&
          normal.credential_result == CN_CREDENTIAL_INVALID &&
          client_init_calls == 1 && sync_calls == 1 &&
          push_calls == 0 && pull_calls == 0 && settings_loads == 0 &&
          credential_loads == 0 && sync_inputs_ok &&
          !strcmp(client_base_url, TEST_BASE_URL),
          "profile normal sync resolves inputs without persistence loads");

    push = cn_sync_push_local_current_book_with_profile(&value, &runtime,
                                                        progress, document);
    check(push.stage == CN_SYNC_PUSH_EXECUTED &&
          push.outcome == CN_SYNC_PUSH_UPLOADED_OUTCOME &&
          push.remote_mutation == CN_KOSYNC_MUTATION_CONFIRMED &&
          push.settings_result == CN_SETTINGS_INVALID_ARGUMENT &&
          push.credential_result == CN_CREDENTIAL_INVALID &&
          client_init_calls == 2 && sync_calls == 1 && push_calls == 1 &&
          pull_calls == 0 && settings_loads == 0 && credential_loads == 0 &&
          push_inputs_ok,
          "profile push preserves PUT-only result semantics without loads");

    pull = cn_sync_pull_remote_current_book_with_profile(&value, &runtime,
                                                         progress, document);
    check(pull.stage == CN_SYNC_PULL_EXECUTED &&
          pull.outcome == CN_SYNC_PULL_PERSISTED_OUTCOME &&
          pull.local_mutation == CN_KOSYNC_MUTATION_CONFIRMED &&
          pull.settings_result == CN_SETTINGS_INVALID_ARGUMENT &&
          pull.credential_result == CN_CREDENTIAL_INVALID &&
          client_init_calls == 3 && sync_calls == 1 && push_calls == 1 &&
          pull_calls == 1 && settings_loads == 0 && credential_loads == 0 &&
          pull_inputs_ok,
          "profile pull preserves GET/local-save semantics and ignores device id");

    invalid = cn_sync_current_book_with_profile(NULL, &runtime, progress,
                                                 document);
    check(invalid.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED &&
          invalid.product.outcome == CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE &&
          invalid.stage != CN_SYNC_CONTROLLER_DISABLED &&
          settings_loads == 0 && credential_loads == 0 && sync_calls == 1,
          "null profile fails as configuration without persistence access");

    invalid = cn_sync_current_book_with_profile(&value, NULL, progress,
                                                 document);
    check(invalid.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED &&
          invalid.product.outcome == CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE &&
          settings_loads == 0 && credential_loads == 0 && sync_calls == 1,
          "null runtime fails before network");

    value.device_name[0] = '\0';
    invalid = cn_sync_current_book_with_profile(&value, &runtime, progress,
                                                 document);
    check(invalid.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED &&
          invalid.product.outcome == CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE &&
          settings_loads == 0 && credential_loads == 0 && sync_calls == 1,
          "empty profile field fails as configuration");

    strcpy(value.device_name, TEST_DEVICE);
    strcpy(value.base_url, "http://profile.synthetic.invalid");
    invalid = cn_sync_current_book_with_profile(&value, &runtime, progress,
                                                 document);
    check(invalid.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED &&
          invalid.product.outcome == CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE &&
          settings_loads == 0 && credential_loads == 0 && sync_calls == 1,
          "malformed base URL fails before operation");

    strcpy(value.base_url, TEST_BASE_URL);
    tls.ca_path = "";
    invalid = cn_sync_current_book_with_profile(&value, &runtime, progress,
                                                 document);
    check(invalid.stage == CN_SYNC_CONTROLLER_CONFIG_FAILED &&
          invalid.product.outcome == CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE &&
          settings_loads == 0 && credential_loads == 0 && sync_calls == 1,
          "invalid TLS runtime fails before network");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "--smoke") != 0) {
        fprintf(stderr, "usage: %s --smoke\n", argv[0]);
        return 2;
    }
    return smoke();
}
