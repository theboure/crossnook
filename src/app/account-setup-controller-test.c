/* Deterministic account setup controller matrix and guarded-card diagnostic. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "account/account_setup_controller.h"
#include "platform/storage_verify.h"

#define TEST_URL "https://setup.synthetic.invalid/base"
#define TEST_URL_TWO "https://setup-two.synthetic.invalid/base"
#define TEST_DEVICE "Setup synthetic device"
#define TEST_DEVICE_TWO "Setup synthetic device two"
#define TEST_USER "setup-synthetic-user"
#define TEST_PASSWORD "setup-synthetic-password"
#define TEST_PASSWORD_TWO "setup-synthetic-password-two"
#define TEST_WRONG_PASSWORD "setup-synthetic-wrong-password"

enum save_fault {
    SAVE_NONE,
    SAVE_FAIL,
    SAVE_DURABILITY
};

static int failures;
static int physical_mode;
static enum save_fault settings_fault;
static enum save_fault credential_fault;
static cn_device_identity_result identity_create_fault = CN_DEVICE_ID_RESULT_COUNT;
static cn_time_result fake_time_result = CN_TIME_OK;
static cn_dns_result fake_dns_result = CN_DNS_OK;
static cn_kosync_result fake_auth_result = CN_KOSYNC_OK;
static cn_persisted_sync_profile_status profile_fault =
    CN_PERSISTED_SYNC_PROFILE_STATUS_COUNT;
static int settings_saves;
static int credential_saves;
static int credential_loads;
static int identity_loads;
static int identity_creates;
static int auth_calls;
static int progress_get_calls;
static int progress_put_calls;
static int userkey_derivations;

cn_settings_result __real_cn_settings_save(const cn_settings_store *,
                                           const cn_settings *, int *);
cn_credential_result __real_cn_credential_store_save(
    const cn_credential_store *, const cn_credentials *, int *);
cn_credential_result __real_cn_credential_store_load(
    const cn_credential_store *, cn_credentials *, int *);
cn_device_identity_result __real_cn_device_identity_load(
    const cn_device_identity_store *, char *, size_t, int *);
cn_device_identity_result __real_cn_device_identity_load_or_create(
    const cn_device_identity_store *, char *, size_t, int *);
cn_kosync_userkey_result __real_cn_kosync_userkey_from_password(
    const unsigned char *, size_t, char *, size_t);
cn_time_result __real_cn_timesimple_sync(const cn_time_config *, cn_time_sample *);
cn_dns_result __real_cn_dnssimple_resolve_a(const cn_dns_config *, const char *,
                                            cn_dns_answer *);
cn_kosync_result __real_cn_kosync_authorize(const cn_kosync_client *,
                                            cn_kosync_outcome *);
cn_kosync_result __real_cn_kosync_get_progress(const cn_kosync_client *,
                                               const char *, cn_kosync_progress *,
                                               cn_kosync_outcome *);
cn_kosync_result __real_cn_kosync_put_progress(const cn_kosync_client *,
                                               const cn_kosync_progress *,
                                               long long *, cn_kosync_outcome *);
cn_persisted_sync_profile_report __real_cn_persisted_sync_profile_load(
    const cn_storage_layout *, cn_persisted_sync_profile *);

cn_settings_result __wrap_cn_settings_save(const cn_settings_store *store,
                                            const cn_settings *settings,
                                            int *error)
{
    cn_settings_result result;
    ++settings_saves;
    if (settings_fault == SAVE_FAIL)
        return CN_SETTINGS_IO_ERROR;
    result = __real_cn_settings_save(store, settings, error);
    return settings_fault == SAVE_DURABILITY && result == CN_SETTINGS_OK
               ? CN_SETTINGS_DURABILITY_UNCERTAIN : result;
}

cn_credential_result __wrap_cn_credential_store_save(
    const cn_credential_store *store, const cn_credentials *credentials,
    int *error)
{
    cn_credential_result result;
    ++credential_saves;
    if (credential_fault == SAVE_FAIL)
        return CN_CREDENTIAL_IO_ERROR;
    result = __real_cn_credential_store_save(store, credentials, error);
    return credential_fault == SAVE_DURABILITY && result == CN_CREDENTIAL_OK
               ? CN_CREDENTIAL_DURABILITY_UNCERTAIN : result;
}

cn_credential_result __wrap_cn_credential_store_load(
    const cn_credential_store *store, cn_credentials *credentials, int *error)
{
    ++credential_loads;
    return __real_cn_credential_store_load(store, credentials, error);
}

cn_device_identity_result __wrap_cn_device_identity_load(
    const cn_device_identity_store *store, char *output, size_t capacity,
    int *error)
{
    ++identity_loads;
    return __real_cn_device_identity_load(store, output, capacity, error);
}

cn_device_identity_result __wrap_cn_device_identity_load_or_create(
    const cn_device_identity_store *store, char *output, size_t capacity,
    int *error)
{
    ++identity_creates;
    if (identity_create_fault != CN_DEVICE_ID_RESULT_COUNT)
        return identity_create_fault;
    return __real_cn_device_identity_load_or_create(store, output, capacity,
                                                    error);
}

cn_kosync_userkey_result __wrap_cn_kosync_userkey_from_password(
    const unsigned char *password, size_t length, char *output, size_t capacity)
{
    ++userkey_derivations;
    return __real_cn_kosync_userkey_from_password(password, length, output,
                                                  capacity);
}

cn_time_result __wrap_cn_timesimple_sync(const cn_time_config *config,
                                         cn_time_sample *sample)
{
    if (physical_mode)
        return __real_cn_timesimple_sync(config, sample);
    (void)config;
    if (sample)
        memset(sample, 0, sizeof *sample);
    return fake_time_result;
}

cn_dns_result __wrap_cn_dnssimple_resolve_a(const cn_dns_config *config,
                                            const char *hostname,
                                            cn_dns_answer *answer)
{
    if (physical_mode)
        return __real_cn_dnssimple_resolve_a(config, hostname, answer);
    (void)config;
    (void)hostname;
    if (fake_dns_result != CN_DNS_OK)
        return fake_dns_result;
    if (answer) {
        memset(answer, 0, sizeof *answer);
        strcpy(answer->ipv4[0], "127.0.0.1");
        answer->count = 1;
    }
    return CN_DNS_OK;
}

cn_kosync_result __wrap_cn_kosync_authorize(const cn_kosync_client *client,
                                            cn_kosync_outcome *outcome)
{
    ++auth_calls;
    if (physical_mode)
        return __real_cn_kosync_authorize(client, outcome);
    if (outcome) {
        outcome->http_status = fake_auth_result == CN_KOSYNC_AUTH_FAILED
                                   ? 401 : 200;
        outcome->transport_result = CN_NETSIMPLE_OK;
    }
    return fake_auth_result;
}

cn_kosync_result __wrap_cn_kosync_get_progress(const cn_kosync_client *client,
                                               const char *document,
                                               cn_kosync_progress *progress,
                                               cn_kosync_outcome *outcome)
{
    (void)client; (void)document; (void)progress; (void)outcome;
    ++progress_get_calls;
    return CN_KOSYNC_INVALID;
}

cn_kosync_result __wrap_cn_kosync_put_progress(const cn_kosync_client *client,
                                               const cn_kosync_progress *progress,
                                               long long *timestamp,
                                               cn_kosync_outcome *outcome)
{
    (void)client; (void)progress; (void)timestamp; (void)outcome;
    ++progress_put_calls;
    return CN_KOSYNC_INVALID;
}

cn_persisted_sync_profile_report __wrap_cn_persisted_sync_profile_load(
    const cn_storage_layout *layout, cn_persisted_sync_profile *profile)
{
    cn_persisted_sync_profile_report report;
    if (profile_fault == CN_PERSISTED_SYNC_PROFILE_STATUS_COUNT)
        return __real_cn_persisted_sync_profile_load(layout, profile);
    memset(&report, 0, sizeof report);
    report.status = profile_fault;
    report.settings_result = CN_SETTINGS_OK;
    report.credential_result = CN_CREDENTIAL_OK;
    report.identity_result = CN_DEVICE_ID_OK;
    if (profile)
        cn_persisted_sync_profile_clear(profile);
    return report;
}

static void check(int condition, const char *name)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        ++failures;
}

static void reset_faults(void)
{
    settings_fault = SAVE_NONE;
    credential_fault = SAVE_NONE;
    identity_create_fault = CN_DEVICE_ID_RESULT_COUNT;
    fake_time_result = CN_TIME_OK;
    fake_dns_result = CN_DNS_OK;
    fake_auth_result = CN_KOSYNC_OK;
    profile_fault = CN_PERSISTED_SYNC_PROFILE_STATUS_COUNT;
    settings_saves = 0;
    credential_saves = 0;
    credential_loads = 0;
    identity_loads = 0;
    identity_creates = 0;
    auth_calls = 0;
    progress_get_calls = 0;
    progress_put_calls = 0;
    userkey_derivations = 0;
}

static int mkdir_root(const char *workspace, unsigned number, char *root,
                      size_t capacity, cn_storage_layout *layout)
{
    int written = snprintf(root, capacity, "%s/root-%u", workspace, number);
    if (written < 0 || (size_t)written >= capacity || mkdir(root, 0700) != 0 ||
        cn_storage_layout_init(layout, root, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(layout, NULL) != CN_STORAGE_OK)
        return 0;
    return 1;
}

static int config_path(const cn_storage_layout *layout, char *path,
                       size_t capacity)
{
    char directory[CN_STORAGE_PATH_CAPACITY];
    int written;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG, directory,
                               sizeof directory) != CN_STORAGE_OK)
        return 0;
    written = snprintf(path, capacity, "%s/settings.conf", directory);
    return written >= 0 && (size_t)written < capacity;
}

static int credentials_path(const cn_storage_layout *layout, char *path,
                            size_t capacity)
{
    char progress[CN_STORAGE_PATH_CAPACITY];
    size_t length;
    int written;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_PROGRESS, progress,
                               sizeof progress) != CN_STORAGE_OK)
        return 0;
    length = strlen(progress);
    if (length <= sizeof "/progress" - 1)
        return 0;
    progress[length - (sizeof "/progress" - 1)] = '\0';
    written = snprintf(path, capacity, "%s/credentials", progress);
    return written >= 0 && (size_t)written < capacity;
}

static int identity_path(const cn_storage_layout *layout, char *path,
                         size_t capacity)
{
    char directory[CN_STORAGE_PATH_CAPACITY];
    int written;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG, directory,
                               sizeof directory) != CN_STORAGE_OK)
        return 0;
    written = snprintf(path, capacity, "%s/device-id", directory);
    return written >= 0 && (size_t)written < capacity;
}

static int write_file(const char *path, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    ssize_t count;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 0;
    while (done < length) {
        count = write(fd, bytes + done, length - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            (void)close(fd);
            return 0;
        }
        done += (size_t)count;
    }
    return close(fd) == 0;
}

static int snapshot(const char *path, unsigned char *output, size_t capacity,
                    size_t *length)
{
    size_t done = 0;
    ssize_t count;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    while (done < capacity) {
        count = read(fd, output + done, capacity - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            (void)close(fd);
            return 0;
        }
        if (count == 0) {
            (void)close(fd);
            *length = done;
            return 1;
        }
        done += (size_t)count;
    }
    (void)close(fd);
    return 0;
}

static int seed_disabled(const cn_storage_layout *layout, const char *url,
                         const char *device, const char *password)
{
    cn_account_bootstrap_report report = cn_account_bootstrap(
        layout, url, device, TEST_USER, (const unsigned char *)password,
        strlen(password), NULL);
    return report.result == CN_ACCOUNT_BOOTSTRAP_OK;
}

static int seed_settings_only(const cn_storage_layout *layout, const char *url,
                              const char *device)
{
    char path[CN_STORAGE_PATH_CAPACITY];
    cn_settings_store store;
    cn_settings settings;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG, path,
                               sizeof path) != CN_STORAGE_OK)
        return 0;
    if (cn_settings_store_init(&store, path, NULL) != CN_SETTINGS_OK)
        return 0;
    cn_settings_defaults(&settings);
    settings.kosync_enabled = 0;
    strcpy(settings.kosync_base_url, url);
    strcpy(settings.kosync_device_name, device);
    return __real_cn_settings_save(&store, &settings, NULL) == CN_SETTINGS_OK;
}

static int seed_orphan(const cn_storage_layout *layout)
{
    char path[CN_STORAGE_PATH_CAPACITY];
    size_t length;
    cn_credential_store store;
    cn_credentials credentials;
    if (!credentials_path(layout, path, sizeof path))
        return 0;
    length = strlen(path);
    if (length <= sizeof "/credentials" - 1)
        return 0;
    path[length - (sizeof "/credentials" - 1)] = '\0';
    if (
        cn_credential_store_init(&store, path, NULL) != CN_CREDENTIAL_OK)
        return 0;
    memset(&credentials, 0, sizeof credentials);
    strcpy(credentials.username, TEST_USER);
    strcpy(credentials.userkey, "0123456789abcdef0123456789abcdef");
    return __real_cn_credential_store_save(&store, &credentials, NULL) ==
           CN_CREDENTIAL_OK;
}

static int runtime_init(cn_sync_activation_runtime *runtime,
                        cn_dns_config *dns, cn_tls_config *tls,
                        cn_time_config *time_config)
{
    memset(dns, 0, sizeof *dns);
    memset(tls, 0, sizeof *tls);
    memset(time_config, 0, sizeof *time_config);
    dns->servers[0] = "synthetic-dns";
    dns->server_count = 1;
    tls->ca_path = "synthetic-ca.pem";
    time_config->servers[0] = "synthetic-time";
    time_config->server_count = 1;
    runtime->dns = dns;
    runtime->tls = tls;
    runtime->time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    runtime->time = time_config;
    return 1;
}

static int enabled_setting(const cn_storage_layout *layout)
{
    char directory[CN_STORAGE_PATH_CAPACITY];
    cn_settings_store store;
    cn_settings settings;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG, directory,
                                sizeof directory) != CN_STORAGE_OK ||
        cn_settings_store_init(&store, directory, NULL) != CN_SETTINGS_OK ||
        cn_settings_load(&store, &settings, NULL) != CN_SETTINGS_OK)
        return 0;
    return settings.kosync_enabled != 0;
}

static cn_account_setup_input input(const char *url, const char *device,
                                    const char *password)
{
    cn_account_setup_input value;
    value.base_url = url;
    value.device_name = device;
    value.username = TEST_USER;
    value.password = (const unsigned char *)password;
    value.password_length = strlen(password);
    return value;
}

static int smoke(void)
{
    char workspace_template[] = "/tmp/crossnook-account-setup.XXXXXX";
    char *workspace = mkdtemp(workspace_template);
    char root[CN_STORAGE_PATH_CAPACITY];
    char path[CN_STORAGE_PATH_CAPACITY];
    unsigned char before[4096], after[4096];
    size_t before_length, after_length;
    cn_storage_layout layout;
    cn_account_setup_input account_input;
    cn_account_setup_report report;
    cn_sync_activation_runtime runtime;
    cn_dns_config dns;
    cn_tls_config tls;
    cn_time_config time_config;

    if (!workspace || !runtime_init(&runtime, &dns, &tls, &time_config))
        return 1;

    reset_faults();
    check(mkdir_root(workspace, 1, root, sizeof root, &layout),
          "fresh root prepared");
    account_input = input(TEST_URL, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ACTIVATED &&
              report.profile_check_attempted &&
              report.profile.status == CN_PERSISTED_SYNC_PROFILE_READY,
          "fresh account reaches enabled READY profile");
    check(report.bootstrap_attempted && report.identity_attempted &&
              report.activation_attempted && auth_calls == 1 &&
              progress_get_calls == 0 && progress_put_calls == 0,
          "fresh flow uses bootstrap identity auth only");

    reset_faults();
    report = cn_account_setup_submit(&layout, NULL,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ALREADY_ENABLED &&
              credential_loads == 0 && identity_loads == 0 &&
              auth_calls == 0 && settings_saves == 0 &&
              userkey_derivations == 0 && !report.profile_check_attempted,
          "enabled fast path loads no secrets or profile");

    reset_faults();
    check(mkdir_root(workspace, 2, root, sizeof root, &layout),
          "rejected root prepared");
    account_input = input(TEST_URL, TEST_DEVICE, TEST_WRONG_PASSWORD);
    fake_auth_result = CN_KOSYNC_AUTH_FAILED;
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_AUTH_REJECTED &&
              report.activation.remote_auth_attempted &&
              !report.activation.settings_enable_visible_possible,
          "wrong credentials leave account disabled");
    reset_faults();
    account_input = input(TEST_URL, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_REPLACE_DISABLED, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ACTIVATED,
          "explicit disabled credential correction succeeds");

    reset_faults();
    check(mkdir_root(workspace, 3, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD),
          "complete disabled root seeded");
    reset_faults();
    if (!credentials_path(&layout, path, sizeof path) ||
        !snapshot(path, before, sizeof before, &before_length))
        return 1;
    account_input = input(TEST_URL, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_PRECONDITION &&
              settings_saves == 0 && credential_saves == 0,
          "new intent does not mutate complete disabled account");
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ACTIVATED &&
              snapshot(path, after, sizeof after, &after_length) &&
              before_length == after_length &&
              !memcmp(before, after, before_length),
          "activate-existing preserves account material");

    reset_faults();
    check(mkdir_root(workspace, 4, root, sizeof root, &layout) &&
              seed_settings_only(&layout, TEST_URL, TEST_DEVICE),
          "settings-only partial root seeded");
    account_input = input(TEST_URL, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ACTIVATED &&
              report.bootstrap.credentials_written,
          "compatible partial bootstrap resumes");

    reset_faults();
    check(mkdir_root(workspace, 5, root, sizeof root, &layout) &&
              seed_orphan(&layout), "orphan credential root seeded");
    account_input = input(TEST_URL, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_INCOMPLETE_STATE &&
              credential_saves == 0, "orphan credentials require recovery");

    reset_faults();
    check(mkdir_root(workspace, 6, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD),
          "retry root seeded");
    fake_dns_result = CN_DNS_TIMEOUT;
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED,
          "DNS failure is retryable without bootstrap");
    reset_faults();
    fake_time_result = CN_TIME_TIMEOUT;
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED,
          "trusted-time failure is retryable");
    reset_faults();
    fake_auth_result = CN_KOSYNC_HTTP_ERROR;
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED,
          "service failure is retryable");

    reset_faults();
    check(mkdir_root(workspace, 7, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD),
          "correction root seeded");
    account_input = input(TEST_URL_TWO, TEST_DEVICE_TWO, TEST_PASSWORD_TWO);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_REPLACE_DISABLED, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ACTIVATED,
          "disabled correction can change account material and configuration");
    reset_faults();
    account_input = input(TEST_URL, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_REPLACE_DISABLED, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ALREADY_ENABLED &&
              settings_saves == 0 && credential_saves == 0,
          "correction is refused once enabled");

    reset_faults();
    check(mkdir_root(workspace, 8, root, sizeof root, &layout),
          "bootstrap failure root prepared");
    settings_fault = SAVE_FAIL;
    account_input = input(TEST_URL, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_BOOTSTRAP_FAILED &&
              !report.identity_attempted && !report.activation_attempted,
          "ordinary bootstrap failure stops later phases");
    reset_faults();
    check(mkdir_root(workspace, 9, root, sizeof root, &layout),
          "bootstrap uncertainty root prepared");
    settings_fault = SAVE_DURABILITY;
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN &&
              !report.identity_attempted && !report.activation_attempted,
          "bootstrap uncertainty stops identity and activation");

    reset_faults();
    check(mkdir_root(workspace, 10, root, sizeof root, &layout),
          "identity failure root prepared");
    identity_create_fault = CN_DEVICE_ID_IO_ERROR;
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_IDENTITY_FAILED &&
              !report.activation_attempted,
          "identity failure stops activation");
    reset_faults();
    check(mkdir_root(workspace, 11, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD),
          "activation failure root seeded");
    settings_fault = SAVE_FAIL;
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ACTIVATION_SAVE_FAILED,
          "activation save failure is reported separately");
    reset_faults();
    check(mkdir_root(workspace, 12, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD),
          "activation uncertainty root seeded");
    settings_fault = SAVE_DURABILITY;
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN &&
              report.activation.remote_auth_succeeded,
          "activation uncertainty does not retry in-call");

    reset_faults();
    check(mkdir_root(workspace, 13, root, sizeof root, &layout),
          "profile failure root prepared");
    profile_fault = CN_PERSISTED_SYNC_PROFILE_VALIDATION_FAILED;
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE &&
              report.profile_check_attempted,
          "enabled profile verification failure is distinct");

    reset_faults();
    check(mkdir_root(workspace, 14, root, sizeof root, &layout) &&
              config_path(&layout, path, sizeof path) &&
              write_file(path, "not-settings\n", sizeof "not-settings\n" - 1),
          "malformed settings root seeded");
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE,
          "malformed settings fail closed");
    reset_faults();
    check(mkdir_root(workspace, 15, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD) &&
              credentials_path(&layout, path, sizeof path) &&
              write_file(path, "not-credentials\n", sizeof "not-credentials\n" - 1),
          "malformed credentials root seeded");
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE,
          "malformed credentials fail closed");
    reset_faults();
    check(mkdir_root(workspace, 16, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD) &&
              identity_path(&layout, path, sizeof path) &&
              write_file(path, "not-identity\n", sizeof "not-identity\n" - 1),
          "malformed identity root seeded");
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE,
          "malformed identity fails closed");
    reset_faults();
    check(mkdir_root(workspace, 17, root, sizeof root, &layout) &&
              seed_settings_only(&layout, TEST_URL, TEST_DEVICE),
          "incompatible partial root seeded");
    account_input = input(TEST_URL_TWO, TEST_DEVICE, TEST_PASSWORD);
    report = cn_account_setup_submit(&layout, &account_input,
                                     CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_PRECONDITION &&
              settings_saves == 0 && credential_saves == 0,
          "partial retry refuses URL and device mutation");
    reset_faults();
    check(mkdir_root(workspace, 18, root, sizeof root, &layout) &&
              config_path(&layout, path, sizeof path) &&
              write_file(path, "crossnook-settings=2\n",
                          sizeof "crossnook-settings=2\n" - 1),
          "unsupported settings root seeded");
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE,
          "unsupported settings fail closed");
    reset_faults();
    check(mkdir_root(workspace, 19, root, sizeof root, &layout) &&
              seed_settings_only(&layout, TEST_URL, TEST_DEVICE) &&
              credentials_path(&layout, path, sizeof path) &&
              write_file(path, "crossnook-credentials=2\n",
                          sizeof "crossnook-credentials=2\n" - 1),
          "unsupported credentials root seeded");
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE,
          "unsupported credentials fail closed");
    reset_faults();
    check(mkdir_root(workspace, 20, root, sizeof root, &layout) &&
              seed_disabled(&layout, TEST_URL, TEST_DEVICE, TEST_PASSWORD) &&
              identity_path(&layout, path, sizeof path) &&
              write_file(path, "crossnook-device-identity=2\n",
                          sizeof "crossnook-device-identity=2\n" - 1),
          "unsupported identity root seeded");
    report = cn_account_setup_activate_existing(&layout, &runtime);
    check(report.status == CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE,
          "unsupported identity fails closed");
    check(progress_get_calls == 0 && progress_put_calls == 0,
          "all setup flows perform zero progress operations");
    return failures != 0;
}

static int physical(int argc, char **argv)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_sync_activation_runtime runtime;
    cn_dns_config dns;
    cn_tls_config tls;
    cn_time_config time_config;
    cn_account_setup_input wrong_input;
    cn_account_setup_input correct_input;
    cn_account_setup_report rejected;
    cn_account_setup_report accepted;
    cn_account_setup_report already;
    unsigned char sentinel_before[4096];
    unsigned char sentinel_after[4096];
    size_t sentinel_before_length;
    size_t sentinel_after_length;
    int rejected_auth;
    int accepted_auth;
    int already_auth;
    int rejected_progress_get, rejected_progress_put;
    int accepted_progress_get, accepted_progress_put;
    int already_progress_get, already_progress_put;
    int enabled;
    int sentinel_stable;
    int error = 0;
    unsigned major, minor;
    char *end;
    const char *mode;
    const char *sentinel;

    if (argc != 14)
        return 2;
    errno = 0; major = (unsigned)strtoul(argv[4], &end, 10);
    if (errno || *end) return 2;
    errno = 0; minor = (unsigned)strtoul(argv[5], &end, 10);
    if (errno || *end) return 2;
    candidate.root = argv[3];
    candidate.mountpoint = argv[2];
    candidate.expected_major = major;
    candidate.expected_minor = minor;
    if (cn_platform_storage_verify(&candidate, &verified, &error) !=
        CN_PLATFORM_STORAGE_OK) {
        printf("ACCOUNT SETUP physical storage=unverified persistence=not-attempted\n");
        return 1;
    }
    sentinel = argv[13];
    if (!snapshot(sentinel, sentinel_before, sizeof sentinel_before,
                  &sentinel_before_length))
        return 1;
    if (cn_storage_layout_init(&layout, verified.root, &error) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(&layout, &error) != CN_STORAGE_OK ||
        !runtime_init(&runtime, &dns, &tls, &time_config))
        return 1;
    physical_mode = 1;
    dns.servers[0] = argv[6];
    dns.port = (unsigned)strtoul(argv[7], NULL, 10);
    time_config.servers[0] = argv[8];
    time_config.port = (unsigned)strtoul(argv[9], NULL, 10);
    tls.ca_path = argv[11];
    wrong_input = input(argv[10], TEST_DEVICE, TEST_WRONG_PASSWORD);
    /* Reuse the synthetic account in testapp/kosync_mock_server.py. */
    correct_input = input(argv[10], TEST_DEVICE, "test-password");
    wrong_input.username = "test-user";
    correct_input.username = "test-user";
    mode = argv[12];
    reset_faults();
    fake_auth_result = CN_KOSYNC_AUTH_FAILED;
    rejected = cn_account_setup_submit(
        &layout, &wrong_input, CN_ACCOUNT_SETUP_NEW_OR_RESUME, &runtime);
    rejected_auth = auth_calls;
    rejected_progress_get = progress_get_calls;
    rejected_progress_put = progress_put_calls;
    if (strcmp(mode, "accepted") != 0 && strcmp(mode, "rejected") != 0)
        return 2;
    if (strcmp(mode, "rejected") == 0) {
        if (!snapshot(sentinel, sentinel_after, sizeof sentinel_after,
                      &sentinel_after_length))
            return 1;
        enabled = enabled_setting(&layout);
        sentinel_stable = sentinel_before_length == sentinel_after_length &&
                          !memcmp(sentinel_before, sentinel_after,
                                  sentinel_before_length);
        printf("ACCOUNT SETUP physical rejected=%s auth_get=%d enabled=%d "
               "progress_get=%d progress_put=%d sentinel_stable=%d\n",
               cn_account_setup_status_name(rejected.status), rejected_auth,
               enabled, rejected_progress_get, rejected_progress_put,
               sentinel_stable);
        return rejected.status == CN_ACCOUNT_SETUP_AUTH_REJECTED &&
                   rejected.activation.kosync_outcome.http_status == 401 &&
                   rejected_auth == 1 && !enabled && rejected_progress_get == 0 &&
                   rejected_progress_put == 0 && sentinel_stable
                   ? 0 : 1;
    }
    reset_faults();
    fake_auth_result = CN_KOSYNC_OK;
    accepted = cn_account_setup_submit(
        &layout, &correct_input, CN_ACCOUNT_SETUP_REPLACE_DISABLED, &runtime);
    accepted_auth = auth_calls;
    accepted_progress_get = progress_get_calls;
    accepted_progress_put = progress_put_calls;
    reset_faults();
    already = cn_account_setup_activate_existing(&layout, &runtime);
    already_auth = auth_calls;
    already_progress_get = progress_get_calls;
    already_progress_put = progress_put_calls;
    if (!snapshot(sentinel, sentinel_after, sizeof sentinel_after,
                  &sentinel_after_length))
        return 1;
    sentinel_stable = sentinel_before_length == sentinel_after_length &&
                      !memcmp(sentinel_before, sentinel_after,
                              sentinel_before_length);
    printf("ACCOUNT SETUP physical rejected=%s accepted=%s already=%s "
           "auth_get=%d already_auth=%d progress_get=%d progress_put=%d "
           "sentinel_stable=%d\n",
           cn_account_setup_status_name(rejected.status),
           cn_account_setup_status_name(accepted.status),
           cn_account_setup_status_name(already.status),
           rejected_auth + accepted_auth, already_auth,
           rejected_progress_get + accepted_progress_get + already_progress_get,
           rejected_progress_put + accepted_progress_put + already_progress_put,
           sentinel_stable);
    return accepted.status == CN_ACCOUNT_SETUP_ACTIVATED &&
           already.status == CN_ACCOUNT_SETUP_ALREADY_ENABLED &&
           rejected.status == CN_ACCOUNT_SETUP_AUTH_REJECTED &&
           rejected.activation.kosync_outcome.http_status == 401 &&
           accepted.activation.kosync_outcome.http_status == 200 &&
           rejected_auth == 1 && accepted_auth == 1 && already_auth == 0 &&
           rejected_progress_get == 0 && rejected_progress_put == 0 &&
           accepted_progress_get == 0 && accepted_progress_put == 0 &&
           already_progress_get == 0 && already_progress_put == 0 &&
           sentinel_stable ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--physical"))
        return physical(argc, argv);
    return smoke();
}
