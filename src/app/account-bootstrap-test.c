/* Synthetic host matrix and verified-card account bootstrap diagnostic. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "account/account_bootstrap.h"
#include "platform/storage_verify.h"

#define SYNTHETIC_USERNAME "test-user"
#define SYNTHETIC_PASSWORD "test-password"
#define SYNTHETIC_USERKEY "dfb450efddbb5387197c84460623675b"
#define SYNTHETIC_URL "https://sync.example.test/base"
#define SYNTHETIC_DEVICE "CrossNook Test"

static int failures;
static int settings_save_calls;
static int credential_save_calls;
static cn_settings_result forced_settings_result;
static cn_credential_result forced_credential_result;

cn_settings_result __real_cn_settings_save(
    const cn_settings_store *, const cn_settings *, int *);
cn_credential_result __real_cn_credential_store_save(
    const cn_credential_store *, const cn_credentials *, int *);

cn_settings_result __wrap_cn_settings_save(
    const cn_settings_store *store, const cn_settings *settings, int *error)
{
    ++settings_save_calls;
    if (forced_settings_result != CN_SETTINGS_OK)
        return forced_settings_result;
    return __real_cn_settings_save(store, settings, error);
}

cn_credential_result __wrap_cn_credential_store_save(
    const cn_credential_store *store, const cn_credentials *credentials,
    int *error)
{
    ++credential_save_calls;
    if (forced_credential_result != CN_CREDENTIAL_OK)
        return forced_credential_result;
    return __real_cn_credential_store_save(store, credentials, error);
}

static void check(int condition, const char *name)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        ++failures;
}

static void reset_faults(void)
{
    settings_save_calls = 0;
    credential_save_calls = 0;
    forced_settings_result = CN_SETTINGS_OK;
    forced_credential_result = CN_CREDENTIAL_OK;
}

static int join_path(char *output, size_t capacity, const char *base,
                     const char *suffix)
{
    size_t base_length = strlen(base);
    size_t suffix_length = strlen(suffix);
    if (base_length + suffix_length + 1 > capacity)
        return 0;
    memcpy(output, base, base_length);
    memcpy(output + base_length, suffix, suffix_length + 1);
    return 1;
}

static int write_raw(const char *path, const char *text)
{
    size_t length = strlen(text), done = 0;
    ssize_t written;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 0;
    while (done < length) {
        written = write(fd, text + done, length - done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            (void)close(fd);
            return 0;
        }
        done += (size_t)written;
    }
    return close(fd) == 0;
}

static int read_contains(const char *path, const char *needle)
{
    unsigned char data[4096];
    size_t done = 0;
    ssize_t got;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    while (done + 1 < sizeof data) {
        got = read(fd, data + done, sizeof data - done - 1);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        done += (size_t)got;
    }
    (void)close(fd);
    data[done] = '\0';
    return strstr((const char *)data, needle) != NULL;
}

static int prepare(const char *root, cn_storage_layout *layout,
                   cn_settings_store *settings_store,
                   cn_credential_store *credential_store)
{
    char config[CN_STORAGE_PATH_CAPACITY];
    char progress[CN_STORAGE_PATH_CAPACITY];
    char state[CN_STORAGE_PATH_CAPACITY];
    size_t length;
    if (cn_storage_layout_init(layout, root, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(layout, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG, config,
                               sizeof config) != CN_STORAGE_OK ||
        cn_storage_layout_path(layout, CN_STORAGE_LOCATION_PROGRESS, progress,
                               sizeof progress) != CN_STORAGE_OK)
        return 0;
    length = strlen(progress) - strlen("/progress");
    memcpy(state, progress, length);
    state[length] = '\0';
    return cn_settings_store_init(settings_store, config, NULL) == CN_SETTINGS_OK &&
           cn_credential_store_init(credential_store, state, NULL) ==
               CN_CREDENTIAL_OK;
}

static int settings_path(const cn_storage_layout *layout, char *path,
                         size_t capacity)
{
    char config[CN_STORAGE_PATH_CAPACITY];
    return cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG, config,
                                  sizeof config) == CN_STORAGE_OK &&
           join_path(path, capacity, config, "/settings.conf");
}

static int credential_path(const cn_storage_layout *layout, char *path,
                           size_t capacity)
{
    char progress[CN_STORAGE_PATH_CAPACITY];
    size_t length;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_PROGRESS, progress,
                               sizeof progress) != CN_STORAGE_OK)
        return 0;
    length = strlen(progress) - strlen("/progress");
    progress[length] = '\0';
    return join_path(path, capacity, progress, "/credentials");
}

static cn_account_bootstrap_report bootstrap(const cn_storage_layout *layout,
                                             const char *url,
                                             const char *device,
                                             const char *username,
                                             const char *password)
{
    return cn_account_bootstrap(layout, url, device, username,
                                (const unsigned char *)password,
                                strlen(password), NULL);
}

static int smoke(void)
{
    char root_template[] = "/tmp/crossnook-account-bootstrap.XXXXXX";
    char root_one[CN_STORAGE_PATH_CAPACITY];
    char root_two[CN_STORAGE_PATH_CAPACITY];
    char root_three[CN_STORAGE_PATH_CAPACITY];
    char settings_file[CN_STORAGE_PATH_CAPACITY];
    char credential_file[CN_STORAGE_PATH_CAPACITY];
    char password_copy[sizeof SYNTHETIC_PASSWORD];
    char userkey[CN_ACCOUNT_USERKEY_TEXT_CAPACITY];
    cn_storage_layout layout_one, layout_two, layout_three;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_settings settings;
    cn_credentials credentials;
    cn_account_bootstrap_report report;
    char *workspace;

    workspace = mkdtemp(root_template);
    if (!workspace)
        return 1;
    snprintf(root_one, sizeof root_one, "%s/one", workspace);
    snprintf(root_two, sizeof root_two, "%s/two", workspace);
    snprintf(root_three, sizeof root_three, "%s/three", workspace);
    check(mkdir(root_one, 0700) == 0 && mkdir(root_two, 0700) == 0 &&
          mkdir(root_three, 0700) == 0, "caller roots created");
    reset_faults();
    check(prepare(root_one, &layout_one, &settings_store, &credential_store),
          "prepared layout accepted");
    check(cn_kosync_userkey_from_password(
              (const unsigned char *)SYNTHETIC_PASSWORD,
              strlen(SYNTHETIC_PASSWORD), userkey, sizeof userkey) ==
              CN_KOSYNC_USERKEY_OK &&
          !strcmp(userkey, SYNTHETIC_USERKEY),
          "KOReader MD5 test vector matches");
    strcpy(password_copy, SYNTHETIC_PASSWORD);
    report = bootstrap(&layout_one, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_OK && report.settings_written &&
          report.credentials_written && settings_save_calls == 1 &&
          credential_save_calls == 1,
          "valid bootstrap writes disabled settings then credentials");
    check(!memcmp(password_copy, SYNTHETIC_PASSWORD, sizeof password_copy),
          "caller password buffer is unchanged");
    check(settings_path(&layout_one, settings_file, sizeof settings_file) &&
          credential_path(&layout_one, credential_file, sizeof credential_file) &&
          !read_contains(settings_file, SYNTHETIC_PASSWORD) &&
          !read_contains(credential_file, SYNTHETIC_PASSWORD),
          "persisted artifacts contain no raw password");
    check(cn_settings_load(&settings_store, &settings, NULL) == CN_SETTINGS_OK &&
          !settings.kosync_enabled &&
          !strcmp(settings.kosync_base_url, SYNTHETIC_URL) &&
          !strcmp(settings.kosync_device_name, SYNTHETIC_DEVICE) &&
          cn_credential_store_load(&credential_store, &credentials, NULL) ==
              CN_CREDENTIAL_OK &&
          !strcmp(credentials.username, SYNTHETIC_USERNAME) &&
          !strcmp(credentials.userkey, SYNTHETIC_USERKEY),
          "persisted settings and credentials reopen correctly");
    cn_credentials_clear(&credentials);

    reset_faults();
    report = bootstrap(&layout_one, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_OK &&
          report.settings_written == 0 && report.credentials_written == 0 &&
          settings_save_calls == 0 && credential_save_calls == 0,
          "identical repeated bootstrap performs zero writes");

    reset_faults();
    report = bootstrap(&layout_one, "https://changed.example.test", "Changed",
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_OK && report.settings_written == 1 &&
          report.credentials_written == 0 && settings_save_calls == 1 &&
          credential_save_calls == 0,
          "changed settings write only settings");
    reset_faults();
    report = bootstrap(&layout_one, "https://changed.example.test", "Changed",
                       SYNTHETIC_USERNAME, "second-password");
    check(report.result == CN_ACCOUNT_BOOTSTRAP_OK && report.settings_written == 0 &&
          report.credentials_written == 1 && settings_save_calls == 0 &&
          credential_save_calls == 1,
          "changed credentials write only credentials");

    reset_faults();
    check(prepare(root_two, &layout_two, &settings_store, &credential_store),
          "active-config fixture prepared");
    cn_settings_defaults(&settings);
    settings.kosync_enabled = 1;
    strcpy(settings.kosync_base_url, SYNTHETIC_URL);
    strcpy(settings.kosync_device_name, SYNTHETIC_DEVICE);
    check(__real_cn_settings_save(&settings_store, &settings, NULL) ==
              CN_SETTINGS_OK, "active settings fixture saved");
    reset_faults();
    report = bootstrap(&layout_two, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_EXISTING_ACTIVE &&
          report.stage == CN_ACCOUNT_BOOTSTRAP_STAGE_PREFLIGHT &&
          settings_save_calls == 0 && credential_save_calls == 0,
          "existing enabled configuration refuses before writes");

    reset_faults();
    check(prepare(root_three, &layout_three, &settings_store, &credential_store),
          "failure fixtures prepared");
    check(settings_path(&layout_three, settings_file, sizeof settings_file) &&
          write_raw(settings_file, "bad\n"), "malformed settings fixture saved");
    report = bootstrap(&layout_three, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_EXISTING_SETTINGS_FAILED &&
          settings_save_calls == 0 && credential_save_calls == 0,
          "malformed settings fail before writes");
    unlink(settings_file);
    check(credential_path(&layout_three, credential_file, sizeof credential_file) &&
          write_raw(credential_file, "bad\n"), "malformed credentials fixture saved");
    report = bootstrap(&layout_three, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_EXISTING_CREDENTIALS_FAILED &&
          settings_save_calls == 0 && credential_save_calls == 0,
          "malformed credentials fail before writes");

    unlink(credential_file);
    reset_faults();
    forced_settings_result = CN_SETTINGS_IO_ERROR;
    report = bootstrap(&layout_three, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_SETTINGS_WRITE_FAILED &&
          report.stage == CN_ACCOUNT_BOOTSTRAP_STAGE_SETTINGS &&
          settings_save_calls == 1 && credential_save_calls == 0,
          "settings failure prevents credential write");
    reset_faults();
    forced_settings_result = CN_SETTINGS_DURABILITY_UNCERTAIN;
    report = bootstrap(&layout_three, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_DURABILITY_UNCERTAIN &&
          report.stage == CN_ACCOUNT_BOOTSTRAP_STAGE_SETTINGS &&
          settings_save_calls == 1 && credential_save_calls == 0,
          "settings durability uncertainty prevents credential write");
    reset_faults();
    forced_credential_result = CN_CREDENTIAL_IO_ERROR;
    report = bootstrap(&layout_three, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_CREDENTIALS_WRITE_FAILED &&
          report.stage == CN_ACCOUNT_BOOTSTRAP_STAGE_CREDENTIALS &&
          report.settings_written == 1 && credential_save_calls == 1,
          "credential failure leaves settings write visible and disabled");
    check(cn_settings_load(&settings_store, &settings, NULL) == CN_SETTINGS_OK &&
          !settings.kosync_enabled,
          "credential failure reloads fail-closed settings");

    reset_faults();
    report = cn_account_bootstrap(&layout_three, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                                  NULL, (const unsigned char *)SYNTHETIC_PASSWORD,
                                  strlen(SYNTHETIC_PASSWORD), NULL);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT &&
          settings_save_calls == 0 && credential_save_calls == 0,
          "missing username fails before persistence");
    report = cn_account_bootstrap(&layout_three, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                                  SYNTHETIC_USERNAME,
                                  (const unsigned char *)SYNTHETIC_PASSWORD,
                                  CN_ACCOUNT_PASSWORD_MAX + 1, NULL);
    check(report.result == CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT &&
          settings_save_calls == 0 && credential_save_calls == 0,
          "oversized password fails before persistence");

    printf("ACCOUNT BOOTSTRAP SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int parse_number(const char *text, unsigned *output)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || !text[0] || *end || value > 65535)
        return 0;
    *output = (unsigned)value;
    return 1;
}

static int physical(char **argv)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_settings settings;
    cn_credentials credentials;
    cn_account_bootstrap_report report;
    unsigned major_number, minor_number;
    char settings_file[CN_STORAGE_PATH_CAPACITY];
    char credential_file[CN_STORAGE_PATH_CAPACITY];
    int raw_password_present;

    candidate.mountpoint = argv[2];
    candidate.root = argv[3];
    if (!parse_number(argv[4], &major_number) ||
        !parse_number(argv[5], &minor_number))
        return 2;
    candidate.expected_major = major_number;
    candidate.expected_minor = minor_number;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) !=
        CN_PLATFORM_STORAGE_OK) {
        puts("ACCOUNT BOOTSTRAP GATE storage=unverified persistence=not-attempted "
             "network=not-attempted");
        return 1;
    }
    if (cn_storage_layout_init(&layout, verified.root, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(&layout, NULL) != CN_STORAGE_OK ||
        !prepare(verified.root, &layout, &settings_store, &credential_store))
        return 1;
    report = bootstrap(&layout, SYNTHETIC_URL, SYNTHETIC_DEVICE,
                       SYNTHETIC_USERNAME, SYNTHETIC_PASSWORD);
    if (settings_path(&layout, settings_file, sizeof settings_file) &&
        credential_path(&layout, credential_file, sizeof credential_file))
        raw_password_present = read_contains(settings_file, SYNTHETIC_PASSWORD) ||
                               read_contains(credential_file, SYNTHETIC_PASSWORD);
    else
        raw_password_present = 1;
    if (cn_settings_load(&settings_store, &settings, NULL) != CN_SETTINGS_OK ||
        cn_credential_store_load(&credential_store, &credentials, NULL) !=
            CN_CREDENTIAL_OK)
        return 1;
    cn_credentials_clear(&credentials);
    printf("ACCOUNT BOOTSTRAP GATE storage=verified result=%s stage=%s "
           "settings_written=%d credentials_written=%d enabled=%d "
           "password-persisted=%s network=not-attempted\n",
           cn_account_bootstrap_result_name(report.result),
           cn_account_bootstrap_stage_name(report.stage),
           report.settings_written, report.credentials_written,
           settings.kosync_enabled, raw_password_present ? "yes" : "no");
    return report.result == CN_ACCOUNT_BOOTSTRAP_OK &&
                   !settings.kosync_enabled && !raw_password_present
               ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--smoke") == 0)
        return smoke();
    if (argc == 6 && strcmp(argv[1], "--physical") == 0)
        return physical(argv);
    fprintf(stderr, "usage: account-bootstrap-test --smoke | --physical "
                    "<mount> <root> <major> <minor>\n");
    return 2;
}
