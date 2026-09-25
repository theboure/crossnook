/* Explicit sync activation host and guarded physical diagnostic. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "account/sync_activation.h"
#include "platform/storage_verify.h"

#define TEST_URL "https://activation.synthetic.invalid/kosync"
#define TEST_USER "activation-synthetic-user"
#define TEST_KEY "activation-synthetic-key"
#define WRONG_KEY "activation-synthetic-wrong-key"
#define TEST_DEVICE "Activation synthetic device"

enum save_fault { SAVE_NONE, SAVE_PRE_RENAME, SAVE_DURABILITY };

static enum save_fault save_mode;
static int physical_mode;
static cn_time_result fake_time_result = CN_TIME_OK;
static cn_dns_result fake_dns_result = CN_DNS_OK;
static cn_kosync_result fake_auth_result = CN_KOSYNC_OK;
static cn_netsimple_result fake_transport_result = CN_NETSIMPLE_OK;
static int fake_http_status = 200;
static int auth_calls;
static int progress_get_calls;
static int progress_put_calls;
static int credential_loads;
static int identity_loads;
static int settings_saves;

cn_settings_result __real_cn_settings_save(const cn_settings_store *,
                                           const cn_settings *, int *);
cn_time_result __real_cn_timesimple_sync(const cn_time_config *,
                                         cn_time_sample *);
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
cn_credential_result __real_cn_credential_store_load(
    const cn_credential_store *, cn_credentials *, int *);
cn_device_identity_result __real_cn_device_identity_load(
    const cn_device_identity_store *, char *, size_t, int *);

cn_settings_result __wrap_cn_settings_save(const cn_settings_store *store,
                                           const cn_settings *settings,
                                           int *error)
{
    cn_settings_result result;
    ++settings_saves;
    if (save_mode == SAVE_PRE_RENAME)
        return CN_SETTINGS_IO_ERROR;
    result = __real_cn_settings_save(store, settings, error);
    if (save_mode == SAVE_DURABILITY && result == CN_SETTINGS_OK)
        return CN_SETTINGS_DURABILITY_UNCERTAIN;
    return result;
}

cn_time_result __wrap_cn_timesimple_sync(const cn_time_config *config,
                                         cn_time_sample *sample)
{
    if (physical_mode)
        return __real_cn_timesimple_sync(config, sample);
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
        outcome->http_status = fake_http_status;
        outcome->transport_result = fake_transport_result;
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

static void check(int condition, const char *name, int *failures)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        ++*failures;
}

static void reset_faults(void)
{
    save_mode = SAVE_NONE;
    fake_time_result = CN_TIME_OK;
    fake_dns_result = CN_DNS_OK;
    fake_auth_result = CN_KOSYNC_OK;
    fake_transport_result = CN_NETSIMPLE_OK;
    fake_http_status = 200;
    auth_calls = 0;
    progress_get_calls = 0;
    progress_put_calls = 0;
    credential_loads = 0;
    identity_loads = 0;
    settings_saves = 0;
}

static int entropy(void *context, unsigned char *output, size_t length)
{
    size_t i;
    (void)context;
    for (i = 0; i < length; ++i)
        output[i] = (unsigned char)(i + 1);
    return 0;
}

static int write_bytes(const char *path, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    if (fd < 0)
        return 0;
    while (done < length) {
        ssize_t count = write(fd, bytes + done, length - done);
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

static int read_bytes(const char *path, unsigned char *data, size_t capacity,
                      size_t *length)
{
    size_t done = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    for (;;) {
        ssize_t count;
        if (done == capacity) {
            (void)close(fd);
            return 0;
        }
        count = read(fd, data + done, capacity - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            (void)close(fd);
            return 0;
        }
        if (count == 0)
            break;
        done += (size_t)count;
    }
    if (close(fd) != 0)
        return 0;
    *length = done;
    return 1;
}

static int path(char *output, size_t capacity, const char *root,
                const char *suffix)
{
    int written = snprintf(output, capacity, "%s%s", root, suffix);
    return written >= 0 && (size_t)written < capacity;
}

static unsigned crc32(const unsigned char *data, size_t length)
{
    unsigned crc = 0xffffffffu;
    size_t i;
    int bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (unsigned)-(int)(crc & 1));
    }
    return ~crc;
}

static int raw_settings(const char *root, int enabled, const char *url)
{
    char file[CN_STORAGE_PATH_CAPACITY];
    char body[CN_SETTINGS_FILE_MAX_BYTES];
    int length;
    unsigned crc;
    if (!path(file, sizeof file, root, "/config/settings.conf"))
        return 0;
    length = snprintf(body, sizeof body,
                      "crossnook-settings=1\nkosync.enabled=%s\n"
                      "kosync.base_url=%s\nkosync.device_name=%s\n",
                      enabled ? "true" : "false", url, TEST_DEVICE);
    if (length < 0 || (size_t)length >= sizeof body)
        return 0;
    crc = crc32((const unsigned char *)body, (size_t)length);
    {
        int suffix_length = snprintf(body + length, sizeof body - (size_t)length,
                                     "crc32=%08x\n", crc);
        if (suffix_length < 0 || (size_t)suffix_length >=
                sizeof body - (size_t)length)
            return 0;
        length += suffix_length;
    }
    return length > 0 && (size_t)length < sizeof body &&
           write_bytes(file, body, (size_t)length);
}

static int raw_text(const char *root, const char *suffix, const char *text)
{
    char file[CN_STORAGE_PATH_CAPACITY];
    return path(file, sizeof file, root, suffix) &&
           write_bytes(file, text, strlen(text));
}

static int make_fixture(char *root, size_t capacity, int enabled)
{
    cn_storage_layout layout;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_device_identity_store identity_store;
    cn_settings settings;
    cn_credentials credentials;
    char template_path[] = "/tmp/crossnook-sync-activation-XXXXXX";
    char config[CN_STORAGE_PATH_CAPACITY];
    char state[CN_STORAGE_PATH_CAPACITY];
    char device_id[CN_DEVICE_ID_TEXT_CAPACITY];
    int error = 0;

    if (!mkdtemp(template_path) || strlen(template_path) >= capacity)
        return 0;
    strcpy(root, template_path);
    if (cn_storage_layout_init(&layout, root, &error) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(&layout, &error) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG, config,
                               sizeof config) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS, state,
                               sizeof state) != CN_STORAGE_OK)
        return 0;
    state[strlen(state) - (sizeof "/progress" - 1)] = '\0';
    if (cn_settings_store_init(&settings_store, config, &error) !=
            CN_SETTINGS_OK)
        return 0;
    cn_settings_defaults(&settings);
    settings.kosync_enabled = enabled;
    strcpy(settings.kosync_base_url, TEST_URL);
    strcpy(settings.kosync_device_name, TEST_DEVICE);
    if (__real_cn_settings_save(&settings_store, &settings, &error) !=
        CN_SETTINGS_OK)
        return 0;
    if (cn_credential_store_init(&credential_store, state, &error) !=
            CN_CREDENTIAL_OK)
        return 0;
    memset(&credentials, 0, sizeof credentials);
    strcpy(credentials.username, TEST_USER);
    strcpy(credentials.userkey, TEST_KEY);
    if (cn_credential_store_save(&credential_store, &credentials, &error) !=
        CN_CREDENTIAL_OK)
        return 0;
    if (cn_device_identity_store_init(&identity_store, &layout, entropy, NULL) !=
            CN_DEVICE_ID_OK)
        return 0;
    if (cn_device_identity_load_or_create(&identity_store, device_id,
                                          sizeof device_id,
                                          &error) != CN_DEVICE_ID_CREATED)
        return 0;
    return 1;
}

static void remove_fixture(const char *root)
{
    char file[CN_STORAGE_PATH_CAPACITY];
    if (path(file, sizeof file, root, "/config/settings.conf")) unlink(file);
    if (path(file, sizeof file, root, "/config/device-id")) unlink(file);
    if (path(file, sizeof file, root, "/state/credentials")) unlink(file);
    if (path(file, sizeof file, root, "/state/progress")) rmdir(file);
    if (path(file, sizeof file, root, "/config")) rmdir(file);
    if (path(file, sizeof file, root, "/state")) rmdir(file);
    rmdir(root);
}

static cn_sync_activation_report activate(const char *root)
{
    cn_storage_layout layout;
    cn_dns_config dns = {{"activation.synthetic.dns"}, 1, 53, 100};
    cn_tls_config tls = {"activation.synthetic.ca", NULL, NULL};
    cn_sync_activation_runtime runtime = {
        &dns, &tls, CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED, NULL
    };
    cn_sync_activation_report report;
    int error = 0;
    if (cn_storage_layout_init(&layout, root, &error) != CN_STORAGE_OK)
        memset(&layout, 0, sizeof layout);
    report = cn_sync_activate(&layout, &runtime);
    return report;
}

static int settings_enabled(const char *root, int *enabled)
{
    cn_settings_store store;
    cn_settings settings;
    char config[CN_STORAGE_PATH_CAPACITY];
    int error = 0;
    if (!path(config, sizeof config, root, "/config") ||
        cn_settings_store_init(&store, config, &error) != CN_SETTINGS_OK ||
        cn_settings_load(&store, &settings, &error) != CN_SETTINGS_OK)
        return 0;
    *enabled = settings.kosync_enabled;
    return 1;
}

static int settings_identity_matches(const char *root)
{
    cn_settings_store store;
    cn_settings settings;
    char config[CN_STORAGE_PATH_CAPACITY];
    int error = 0;
    return path(config, sizeof config, root, "/config") &&
           cn_settings_store_init(&store, config, &error) == CN_SETTINGS_OK &&
           cn_settings_load(&store, &settings, &error) == CN_SETTINGS_OK &&
           settings.kosync_enabled &&
           strcmp(settings.kosync_base_url, TEST_URL) == 0 &&
           strcmp(settings.kosync_device_name, TEST_DEVICE) == 0;
}

static int file_snapshot(const char *root, const char *suffix,
                         unsigned char *data, size_t capacity, size_t *length)
{
    char file[CN_STORAGE_PATH_CAPACITY];
    return path(file, sizeof file, root, suffix) &&
           read_bytes(file, data, capacity, length);
}

static int smoke(void)
{
    char root[CN_STORAGE_PATH_CAPACITY];
    unsigned char settings_before[CN_SETTINGS_FILE_MAX_BYTES];
    unsigned char settings_after[CN_SETTINGS_FILE_MAX_BYTES];
    unsigned char credentials_before[CN_CREDENTIAL_FILE_MAX_BYTES];
    unsigned char identity_before[CN_DEVICE_ID_FILE_MAX_BYTES];
    unsigned char identity_after[CN_DEVICE_ID_FILE_MAX_BYTES];
    size_t settings_length, credentials_length, identity_length;
    cn_sync_activation_report report;
    int enabled;
    int failures = 0;

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "disabled fixture", &failures);
    check(file_snapshot(root, "/state/credentials", credentials_before,
                        sizeof credentials_before, &credentials_length) &&
              file_snapshot(root, "/config/device-id", identity_before,
                            sizeof identity_before, &identity_length),
          "capture immutable account files", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_OK && report.remote_auth_succeeded &&
              report.settings_save_attempted && settings_identity_matches(root) &&
              settings_enabled(root, &enabled) && enabled,
          "successful activation preserves URL and device name", &failures);
    check(file_snapshot(root, "/state/credentials", settings_after,
                        sizeof credentials_before, &settings_length) &&
              settings_length == credentials_length &&
              memcmp(settings_after, credentials_before, credentials_length) == 0 &&
              file_snapshot(root, "/config/device-id", identity_after,
                            sizeof identity_after, &settings_length) &&
              settings_length == identity_length &&
              memcmp(identity_after, identity_before, identity_length) == 0,
          "credentials and identity remain byte-stable", &failures);
    check(auth_calls == 1 && progress_get_calls == 0 && progress_put_calls == 0,
          "activation performs one auth GET and no progress operation", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "auth rejection fixture", &failures);
    check(file_snapshot(root, "/config/settings.conf", settings_before,
                        sizeof settings_before, &settings_length) &&
              file_snapshot(root, "/state/credentials", credentials_before,
                            sizeof credentials_before, &credentials_length) &&
              file_snapshot(root, "/config/device-id", identity_before,
                            sizeof identity_before, &identity_length),
          "capture disabled state", &failures);
    fake_auth_result = CN_KOSYNC_AUTH_FAILED;
    fake_http_status = 401;
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_AUTH_REJECTED &&
              !report.settings_save_attempted && settings_enabled(root, &enabled) &&
              !enabled,
          "rejected auth leaves disabled settings", &failures);
    check(auth_calls == 1 && progress_get_calls == 0 && progress_put_calls == 0,
          "rejected auth has no progress mutation", &failures);
    check(file_snapshot(root, "/config/settings.conf", settings_after,
                        sizeof settings_after, &identity_length) &&
              identity_length == settings_length &&
              memcmp(settings_after, settings_before, settings_length) == 0 &&
              file_snapshot(root, "/state/credentials", credentials_before,
                            sizeof credentials_before, &credentials_length),
          "rejected auth preserves local files", &failures);
    remove_fixture(root);

    reset_faults();
    fake_dns_result = CN_DNS_NXDOMAIN;
    check(make_fixture(root, sizeof root, 0), "DNS failure fixture", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_DNS_FAILED && auth_calls == 0 &&
              settings_enabled(root, &enabled) && !enabled,
          "DNS failure is fail-closed", &failures);
    remove_fixture(root);

    reset_faults();
    fake_time_result = CN_TIME_TIMEOUT;
    check(make_fixture(root, sizeof root, 0), "trusted-time failure fixture",
          &failures);
    {
        cn_storage_layout layout;
        cn_dns_config dns = {{"activation.synthetic.dns"}, 1, 53, 100};
        cn_tls_config tls = {"activation.synthetic.ca", NULL, NULL};
        cn_time_config time = {{"activation.synthetic.time"}, 1, 123, 100};
        cn_sync_activation_runtime runtime = {
            &dns, &tls, CN_KOSYNC_SYNC_TIME_ESTABLISH, &time
        };
        int error = 0;
        cn_storage_layout_init(&layout, root, &error);
        report = cn_sync_activate(&layout, &runtime);
    }
    check(report.status == CN_SYNC_ACTIVATION_TRUSTED_TIME_FAILED &&
              auth_calls == 0 && settings_enabled(root, &enabled) && !enabled,
          "trusted-time failure is fail-closed", &failures);
    remove_fixture(root);

    reset_faults();
    fake_auth_result = CN_KOSYNC_TRANSPORT_ERROR;
    fake_transport_result = CN_NETSIMPLE_TLS_INVALID_CA;
    fake_http_status = 0;
    check(make_fixture(root, sizeof root, 0), "TLS failure fixture", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_HTTPS_FAILED &&
              settings_enabled(root, &enabled) && !enabled,
          "TLS/CA failure is fail-closed", &failures);
    remove_fixture(root);

    reset_faults();
    fake_auth_result = CN_KOSYNC_HTTP_ERROR;
    fake_http_status = 500;
    check(make_fixture(root, sizeof root, 0), "service failure fixture", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_SERVICE_FAILED &&
              settings_enabled(root, &enabled) && !enabled,
          "service failure is fail-closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 1), "already-enabled fixture", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_ALREADY_ENABLED && auth_calls == 0 &&
              credential_loads == 0 && identity_loads == 0 && settings_saves == 0,
          "already enabled avoids secret loads, network and writes", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "empty URL fixture", &failures);
    check(raw_settings(root, 0, ""), "write disabled empty URL", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_CONFIG_FAILED && auth_calls == 0,
          "empty URL fails local configuration", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "invalid URL fixture", &failures);
    check(raw_settings(root, 0, "http://activation.synthetic.invalid/kosync"),
          "write invalid URL", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_SETTINGS_FAILED && auth_calls == 0,
          "invalid URL fails settings validation", &failures);
    remove_fixture(root);

    reset_faults();
    {
        cn_storage_layout layout;
        char template_path[] = "/tmp/crossnook-sync-activation-empty-XXXXXX";
        int error = 0;
        check(mkdtemp(template_path) != NULL, "missing-settings fixture", &failures);
        strcpy(root, template_path);
        cn_storage_layout_init(&layout, root, &error);
        cn_storage_layout_prepare(&layout, &error);
        report = activate(root);
        check(report.status == CN_SYNC_ACTIVATION_SETTINGS_FAILED &&
                  auth_calls == 0, "missing settings fail closed", &failures);
        remove_fixture(root);
    }

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "corrupt-settings fixture", &failures);
    check(raw_text(root, "/config/settings.conf", "not-settings\n"),
          "write corrupt settings", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_SETTINGS_FAILED,
          "corrupt settings fail closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "unsupported-settings fixture",
          &failures);
    check(raw_text(root, "/config/settings.conf", "crossnook-settings=2\n"),
          "write unsupported settings", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_SETTINGS_FAILED,
          "unsupported settings fail closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "missing-credential fixture",
          &failures);
    {
        char file[CN_STORAGE_PATH_CAPACITY];
        path(file, sizeof file, root, "/state/credentials");
        unlink(file);
    }
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_CREDENTIALS_FAILED &&
              auth_calls == 0, "missing credentials fail closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "corrupt-credential fixture",
          &failures);
    check(raw_text(root, "/state/credentials", "bad-credentials\n"),
          "write corrupt credentials", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_CREDENTIALS_FAILED,
          "corrupt credentials fail closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "unsupported-credential fixture",
          &failures);
    check(raw_text(root, "/state/credentials", "crossnook-credentials=2\n"),
          "write unsupported credentials", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_CREDENTIALS_FAILED,
          "unsupported credentials fail closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "missing-identity fixture", &failures);
    {
        char file[CN_STORAGE_PATH_CAPACITY];
        path(file, sizeof file, root, "/config/device-id");
        unlink(file);
    }
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_IDENTITY_FAILED &&
              auth_calls == 0, "missing identity fails without creation", &failures);
    {
        char file[CN_STORAGE_PATH_CAPACITY];
        path(file, sizeof file, root, "/config/device-id");
        check(access(file, F_OK) != 0, "activation does not create identity", &failures);
    }
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "malformed-identity fixture", &failures);
    check(raw_text(root, "/config/device-id", "bad-identity\n"),
          "write malformed identity", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_IDENTITY_FAILED,
          "malformed identity fails closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "unsupported-identity fixture", &failures);
    check(raw_text(root, "/config/device-id", "crossnook-device-identity=2\n"),
          "write unsupported identity", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_IDENTITY_FAILED,
          "unsupported identity fails closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "bad-checksum identity fixture", &failures);
    check(raw_text(root, "/config/device-id",
                   "crossnook-device-identity=1\nid=0102030405060708090a0b0c0d0e0f10\n"
                   "crc32=00000000\n"), "write bad checksum identity", &failures);
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_IDENTITY_FAILED,
          "bad identity checksum fails closed", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "save-failure fixture", &failures);
    check(file_snapshot(root, "/config/settings.conf", settings_before,
                        sizeof settings_before, &settings_length),
          "capture save-failure settings", &failures);
    save_mode = SAVE_PRE_RENAME;
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_SETTINGS_SAVE_FAILED &&
              report.remote_auth_succeeded && settings_saves == 1 &&
              file_snapshot(root, "/config/settings.conf", settings_after,
                            sizeof settings_after, &identity_length) &&
              identity_length == settings_length &&
              memcmp(settings_after, settings_before, settings_length) == 0,
          "pre-rename save failure preserves disabled settings", &failures);
    save_mode = SAVE_NONE;
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_OK && settings_enabled(root, &enabled) &&
              enabled, "ordinary activation retry succeeds", &failures);
    remove_fixture(root);

    reset_faults();
    check(make_fixture(root, sizeof root, 0), "durability fixture", &failures);
    save_mode = SAVE_DURABILITY;
    report = activate(root);
    check(report.status == CN_SYNC_ACTIVATION_DURABILITY_UNCERTAIN &&
              report.remote_auth_succeeded && report.settings_save_attempted &&
              report.settings_enable_visible_possible && settings_enabled(root, &enabled) &&
              enabled, "post-rename durability uncertainty is reported", &failures);
    {
        int before_auth = auth_calls;
        save_mode = SAVE_NONE;
        report = activate(root);
        check(report.status == CN_SYNC_ACTIVATION_ALREADY_ENABLED &&
                  auth_calls == before_auth && settings_saves == 1,
              "visible uncertain enablement retries idempotently", &failures);
    }
    remove_fixture(root);

    check(auth_calls >= 0 && progress_get_calls == 0 && progress_put_calls == 0,
          "activation suite observed zero progress GET and PUT", &failures);
    printf("SYNC ACTIVATION HOST failures=%d progress_get=%d progress_put=%d -> %s\n",
           failures, progress_get_calls, progress_put_calls,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int physical(int argc, char **argv)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_device_identity_store identity_store;
    cn_settings settings;
    cn_credentials credentials;
    cn_dns_config dns;
    cn_tls_config tls;
    cn_time_config time;
    cn_sync_activation_runtime runtime;
    cn_sync_activation_report report;
    char config[CN_STORAGE_PATH_CAPACITY];
    char state[CN_STORAGE_PATH_CAPACITY];
    char device_id[CN_DEVICE_ID_TEXT_CAPACITY];
    unsigned char credentials_before[CN_CREDENTIAL_FILE_MAX_BYTES];
    unsigned char credentials_after[CN_CREDENTIAL_FILE_MAX_BYTES];
    unsigned char identity_before[CN_DEVICE_ID_FILE_MAX_BYTES];
    unsigned char identity_after[CN_DEVICE_ID_FILE_MAX_BYTES];
    unsigned char sentinel_before[1024];
    unsigned char sentinel_after[1024];
    size_t credentials_length, credentials_after_length;
    size_t identity_length, identity_after_length;
    size_t sentinel_length, sentinel_after_length;
    char *end;
    int error = 0;
    unsigned major, minor;
    int accepted;
    int enabled;

    if (argc != 14) {
        fprintf(stderr, "usage: --physical <mount> <root> <major> <minor> "
                        "<dns> <dns-port> <time> <time-port> <base-url> <ca> "
                        "<accepted|rejected> <outside-sentinel>\n");
        return 2;
    }
    accepted = strcmp(argv[12], "accepted") == 0;
    if (!accepted && strcmp(argv[12], "rejected") != 0)
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
        printf("SYNC ACTIVATION physical storage=unverified persistence=not-attempted\n");
        return 1;
    }
    if (cn_storage_layout_init(&layout, verified.root, &error) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(&layout, &error) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG, config,
                               sizeof config) != CN_STORAGE_OK ||
        cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS, state,
                               sizeof state) != CN_STORAGE_OK)
        return 1;
    state[strlen(state) - (sizeof "/progress" - 1)] = '\0';
    if (cn_settings_store_init(&settings_store, config, &error) != CN_SETTINGS_OK)
        return 1;
    memset(&settings, 0, sizeof settings);
    {
        cn_settings_result load_result = cn_settings_load(&settings_store,
                                                           &settings, &error);
        if (load_result != CN_SETTINGS_MISSING && load_result != CN_SETTINGS_OK)
            return 1;
    }
    if (!settings.kosync_enabled && !settings.kosync_base_url[0]) {
        cn_settings_defaults(&settings);
        settings.kosync_enabled = 0;
        if (strlen(argv[10]) >= sizeof settings.kosync_base_url ||
            strlen(argv[10]) == 0)
            return 1;
        strcpy(settings.kosync_base_url, argv[10]);
        strcpy(settings.kosync_device_name, TEST_DEVICE);
        if (__real_cn_settings_save(&settings_store, &settings, &error) !=
            CN_SETTINGS_OK)
            return 1;
        if (cn_credential_store_init(&credential_store, state, &error) !=
                CN_CREDENTIAL_OK)
            return 1;
        memset(&credentials, 0, sizeof credentials);
        strcpy(credentials.username, TEST_USER);
        strcpy(credentials.userkey,
               strcmp(argv[12], "accepted") == 0 ? TEST_KEY : WRONG_KEY);
        if (cn_credential_store_save(&credential_store, &credentials, &error) !=
            CN_CREDENTIAL_OK)
            return 1;
        if (cn_device_identity_store_init(&identity_store, &layout, entropy, NULL) !=
                CN_DEVICE_ID_OK ||
            cn_device_identity_load_or_create(&identity_store, device_id,
                                              sizeof device_id, &error) !=
                CN_DEVICE_ID_CREATED)
            return 1;
    }
    if (!file_snapshot(verified.root, "/state/credentials", credentials_before,
                       sizeof credentials_before, &credentials_length) ||
        !file_snapshot(verified.root, "/config/device-id", identity_before,
                       sizeof identity_before, &identity_length) ||
        !read_bytes(argv[13], sentinel_before, sizeof sentinel_before,
                    &sentinel_length))
        return 1;
    dns.servers[0] = argv[6]; dns.server_count = 1;
    dns.port = (unsigned)strtoul(argv[7], NULL, 10); dns.timeout_ms = 3000;
    tls.ca_path = argv[11]; tls.entropy_path = NULL; tls.get_time = NULL;
    time.servers[0] = argv[8]; time.server_count = 1;
    time.port = (unsigned)strtoul(argv[9], NULL, 10); time.timeout_ms = 3000;
    runtime.dns = &dns; runtime.tls = &tls;
    runtime.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH; runtime.time = &time;
    physical_mode = 1;
    report = cn_sync_activate(&layout, &runtime);
    if (!file_snapshot(verified.root, "/state/credentials", credentials_after,
                       sizeof credentials_after, &credentials_after_length) ||
        !file_snapshot(verified.root, "/config/device-id", identity_after,
                       sizeof identity_after, &identity_after_length) ||
        !read_bytes(argv[13], sentinel_after, sizeof sentinel_after,
                    &sentinel_after_length) ||
        !settings_enabled(verified.root, &enabled))
        return 1;
    printf("SYNC ACTIVATION physical status=%s auth_get=%d auth_ok=%d saved=%d "
           "visible=%d progress_get=%d progress_put=%d credentials_stable=%d "
           "identity_stable=%d sentinel_stable=%d enabled=%d\n",
           cn_sync_activation_status_name(report.status),
           auth_calls, report.remote_auth_succeeded, report.settings_save_attempted,
           report.settings_enable_visible_possible, progress_get_calls,
           progress_put_calls,
           credentials_length == credentials_after_length &&
               memcmp(credentials_before, credentials_after, credentials_length) == 0,
           identity_length == identity_after_length &&
               memcmp(identity_before, identity_after, identity_length) == 0,
           sentinel_length == sentinel_after_length &&
               memcmp(sentinel_before, sentinel_after, sentinel_length) == 0,
           enabled);
    if (accepted)
        return report.status == CN_SYNC_ACTIVATION_OK ||
                       report.status == CN_SYNC_ACTIVATION_ALREADY_ENABLED ? 0 : 1;
    return report.status == CN_SYNC_ACTIVATION_AUTH_REJECTED && !enabled ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--physical") == 0)
        return physical(argc, argv);
    return smoke();
}
