/* Deterministic host matrix and isolated-card profile diagnostic. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "identity/device_identity.h"
#include "platform/storage_verify.h"
#include "sync/persisted_sync_profile.h"

#define TEST_URL "https://sync.example.test/base"
#define TEST_USERNAME "profile-user"
#define TEST_USERKEY "0123456789abcdef0123456789abcdef"
#define TEST_DEVICE "CrossNook Profile"
#define TEST_ID "00112233445566778899aabbccddeeff"

static int failures;

static void check(int condition, const char *name)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        ++failures;
}

static uint32_t crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    int bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                  (UINT32_C(0xedb88320) & (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
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

static int write_text(const char *path, const char *text)
{
    return write_file(path, text, strlen(text));
}

static int file_bytes(const char *path, unsigned char *output, size_t capacity,
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
            *length = done;
            return close(fd) == 0;
        }
        done += (size_t)count;
    }
    (void)close(fd);
    return 0;
}

static int identity_fixture(const char *path, const char *id,
                            const char *version, int bad_checksum)
{
    char data[CN_DEVICE_ID_FILE_MAX_BYTES];
    size_t prefix;
    int written;
    uint32_t checksum;
    written = snprintf(data, sizeof data, "crossnook-device-identity=%s\n"
                       "id=%s\n", version, id);
    if (written < 0 || (size_t)written >= sizeof data)
        return 0;
    prefix = (size_t)written;
    checksum = crc32((const unsigned char *)data, prefix);
    if (bad_checksum)
        ++checksum;
    written = snprintf(data + prefix, sizeof data - prefix,
                       "crc32=%08x\n", checksum);
    return written > 0 && write_file(path, data, prefix + (size_t)written);
}

static int paths(const cn_storage_layout *layout, char *config,
                 char *credentials, char *identity)
{
    int written;
    char directory[CN_STORAGE_PATH_CAPACITY];
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG,
                               directory, sizeof directory) != CN_STORAGE_OK)
        return 0;
    written = snprintf(config, CN_STORAGE_PATH_CAPACITY,
                       "%s/settings.conf", directory);
    if (written <= 0 || (size_t)written >= CN_STORAGE_PATH_CAPACITY)
        return 0;
    written = snprintf(credentials, CN_STORAGE_PATH_CAPACITY,
                       "%s/state/credentials", layout->root);
    if (written <= 0 || (size_t)written >= CN_STORAGE_PATH_CAPACITY)
        return 0;
    written = snprintf(identity, CN_STORAGE_PATH_CAPACITY,
                       "%s/config/device-id", layout->root);
    return written > 0 && (size_t)written < CN_STORAGE_PATH_CAPACITY;
}

static int stores(const cn_storage_layout *layout, cn_settings_store *settings,
                  cn_credential_store *credentials)
{
    char config[CN_STORAGE_PATH_CAPACITY];
    char state[CN_STORAGE_PATH_CAPACITY];
    int written;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG,
                               config, sizeof config) != CN_STORAGE_OK)
        return 0;
    written = snprintf(state, sizeof state, "%s/state", layout->root);
    return written > 0 && (size_t)written < sizeof state &&
           cn_settings_store_init(settings, config, NULL) == CN_SETTINGS_OK &&
           cn_credential_store_init(credentials, state, NULL) == CN_CREDENTIAL_OK;
}

static int write_settings(const cn_settings_store *store, int enabled)
{
    cn_settings settings;
    cn_settings_defaults(&settings);
    settings.kosync_enabled = enabled;
    strcpy(settings.kosync_base_url, TEST_URL);
    strcpy(settings.kosync_device_name, TEST_DEVICE);
    return cn_settings_save(store, &settings, NULL) == CN_SETTINGS_OK;
}

static int write_credentials(const cn_credential_store *store)
{
    cn_credentials credentials;
    memset(&credentials, 0, sizeof credentials);
    strcpy(credentials.username, TEST_USERNAME);
    strcpy(credentials.userkey, TEST_USERKEY);
    if (cn_credential_store_save(store, &credentials, NULL) != CN_CREDENTIAL_OK)
        return 0;
    cn_credentials_clear(&credentials);
    return 1;
}

static int zero_profile(const cn_persisted_sync_profile *profile)
{
    const unsigned char *bytes = (const unsigned char *)profile;
    size_t i;
    for (i = 0; i < sizeof *profile; ++i)
        if (bytes[i] != 0)
            return 0;
    return 1;
}

static int profile_fixture(cn_storage_layout *layout,
                           cn_settings_store *settings,
                           cn_credential_store *credentials,
                           char identity[CN_STORAGE_PATH_CAPACITY])
{
    char config[CN_STORAGE_PATH_CAPACITY];
    char ignored[CN_STORAGE_PATH_CAPACITY];
    return stores(layout, settings, credentials) &&
           paths(layout, config, ignored, identity) &&
           write_settings(settings, 1) && write_credentials(credentials) &&
           identity_fixture(identity, TEST_ID, "1", 0);
}

static int smoke(void)
{
    char root[] = "/tmp/cn-persisted-profile-XXXXXX";
    char config[CN_STORAGE_PATH_CAPACITY];
    char credentials_path[CN_STORAGE_PATH_CAPACITY];
    char identity_path[CN_STORAGE_PATH_CAPACITY];
    unsigned char settings_before[4096], settings_after[4096];
    unsigned char credentials_before[4096], credentials_after[4096];
    unsigned char identity_before[4096], identity_after[4096];
    size_t settings_before_length, settings_after_length;
    size_t credentials_before_length, credentials_after_length;
    size_t identity_before_length, identity_after_length;
    cn_storage_layout layout;
    cn_settings_store settings;
    cn_credential_store credentials;
    cn_device_identity_store identity_store;
    cn_persisted_sync_profile profile;
    cn_persisted_sync_profile_report report;
    cn_device_identity_result identity_result;
    int first_exists;

    check(mkdtemp(root) != NULL, "temporary root");
    check(cn_storage_layout_init(&layout, root, NULL) == CN_STORAGE_OK &&
          cn_storage_layout_prepare(&layout, NULL) == CN_STORAGE_OK,
          "prepared synthetic layout");
    check(paths(&layout, config, credentials_path, identity_path),
          "derived fixture paths");
    check(stores(&layout, &settings, &credentials), "initialized stores");

    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.status == CN_PERSISTED_SYNC_PROFILE_DISABLED &&
          report.settings_attempted && !report.credentials_attempted &&
          !report.identity_attempted, "missing settings disables without later reads");

    check(write_settings(&settings, 0) && write_text(credentials_path, "bad") &&
          write_text(identity_path, "bad"), "disabled corrupt fixtures");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.status == CN_PERSISTED_SYNC_PROFILE_DISABLED &&
          !report.credentials_attempted && !report.identity_attempted,
          "disabled ignores corrupt credentials and identity");

    check(profile_fixture(&layout, &settings, &credentials, identity_path),
          "complete enabled fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.status == CN_PERSISTED_SYNC_PROFILE_READY &&
          !strcmp(profile.base_url, TEST_URL) &&
          !strcmp(profile.username, TEST_USERNAME) &&
          !strcmp(profile.userkey, TEST_USERKEY) &&
          !strcmp(profile.device_name, TEST_DEVICE) &&
          !strcmp(profile.device_id, TEST_ID),
          "ready profile preserves exact persisted values");
    check(file_bytes(config, settings_before, sizeof settings_before,
                     &settings_before_length) &&
          file_bytes(credentials_path, credentials_before,
                     sizeof credentials_before, &credentials_before_length) &&
          file_bytes(identity_path, identity_before, sizeof identity_before,
                     &identity_before_length),
          "captured persistence fixtures");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.status == CN_PERSISTED_SYNC_PROFILE_READY,
          "repeated profile load remains ready");
    check(file_bytes(config, settings_after, sizeof settings_after,
                     &settings_after_length) &&
          file_bytes(credentials_path, credentials_after,
                     sizeof credentials_after, &credentials_after_length) &&
          file_bytes(identity_path, identity_after, sizeof identity_after,
                     &identity_after_length) &&
          settings_after_length == settings_before_length &&
          credentials_after_length == credentials_before_length &&
          identity_after_length == identity_before_length &&
          !memcmp(settings_before, settings_after, settings_before_length) &&
          !memcmp(credentials_before, credentials_after, credentials_before_length) &&
          !memcmp(identity_before, identity_after, identity_before_length),
          "all persistence unchanged after repeated composition");

    check(unlink(credentials_path) == 0, "remove credentials fixture");
    memset(&profile, 0xa5, sizeof profile);
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.status == CN_PERSISTED_SYNC_PROFILE_CREDENTIALS_FAILED &&
          zero_profile(&profile), "missing credentials clears output");
    check(write_text(credentials_path,
                     "crossnook-credentials=2\nusername=x\nuserkey=y\n"
                     "crc32=00000000\n"),
          "unsupported credentials fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.credential_result == CN_CREDENTIAL_UNSUPPORTED_VERSION &&
          zero_profile(&profile), "unsupported credentials version typed");
    check(write_credentials(&credentials), "restore credentials fixture");
    check(write_text(credentials_path, "malformed credentials\n"),
          "malformed credentials fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.credential_result == CN_CREDENTIAL_CORRUPT &&
          zero_profile(&profile), "malformed credentials typed and cleared");
    check(write_credentials(&credentials), "restore credentials after malformed fixture");

    check(unlink(identity_path) == 0 &&
          cn_device_identity_store_init(&identity_store, &layout, NULL, NULL) ==
              CN_DEVICE_ID_OK,
          "remove identity without creating replacement");
    memset(&profile, 0xa5, sizeof profile);
    identity_result = cn_device_identity_load(&identity_store, profile.device_id,
                                              sizeof profile.device_id, NULL);
    first_exists = access(identity_path, F_OK) == 0;
    check(identity_result == CN_DEVICE_ID_NOT_FOUND && !first_exists,
          "load-only missing identity does not create");
    check(identity_fixture(identity_path, TEST_ID, "1", 0), "restore identity");
    check(identity_fixture(identity_path, "malformed", "1", 0),
          "malformed identity fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.identity_result == CN_DEVICE_ID_MALFORMED &&
          zero_profile(&profile), "malformed identity typed and cleared");
    check(identity_fixture(identity_path, TEST_ID, "1", 1), "bad checksum fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.identity_result == CN_DEVICE_ID_BAD_CHECKSUM,
          "bad identity checksum typed");
    check(identity_fixture(identity_path, TEST_ID, "2", 0),
          "unsupported identity fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.identity_result == CN_DEVICE_ID_UNSUPPORTED_VERSION,
          "unsupported identity version typed");
    check(identity_fixture(identity_path, TEST_ID, "1", 0), "restore valid identity");

    check(write_text(config, "corrupt settings\n"), "corrupt settings fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.status == CN_PERSISTED_SYNC_PROFILE_SETTINGS_FAILED &&
          zero_profile(&profile), "corrupt settings typed and cleared");
    check(write_text(config, "crossnook-settings=2\n"
                           "kosync.enabled=false\n"
                           "kosync.base_url=x\n"
                           "kosync.device_name=x\n"
                           "crc32=00000000\n"),
          "unsupported settings fixture");
    report = cn_persisted_sync_profile_load(&layout, &profile);
    check(report.settings_result == CN_SETTINGS_UNSUPPORTED_VERSION,
          "unsupported settings version typed");

    memset(&profile, 0xa5, sizeof profile);
    cn_persisted_sync_profile_clear(&profile);
    check(zero_profile(&profile), "profile clear wipes complete object");

    (void)unlink(config);
    (void)unlink(credentials_path);
    (void)unlink(identity_path);
    printf("PERSISTED SYNC PROFILE SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int number(const char *text, unsigned *value)
{
    char *end;
    unsigned long parsed;
    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno || !text[0] || *end || parsed > 65535)
        return 0;
    *value = (unsigned)parsed;
    return 1;
}

static int verify_physical(char **argv, int base,
                           cn_platform_storage_verified *verified)
{
    cn_platform_storage_candidate candidate;
    unsigned major_number, minor_number;

    candidate.mountpoint = argv[base];
    candidate.root = argv[base + 1];
    if (!number(argv[base + 2], &major_number) ||
        !number(argv[base + 3], &minor_number))
        return 2;
    candidate.expected_major = major_number;
    candidate.expected_minor = minor_number;
    if (cn_platform_storage_verify(&candidate, verified, NULL) !=
        CN_PLATFORM_STORAGE_OK) {
        puts("PERSISTED SYNC PROFILE GATE storage=unverified persistence=not-attempted network=not-attempted");
        return 1;
    }
    return 0;
}

static int physical_setup(char **argv)
{
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_settings_store settings;
    cn_credential_store credentials;
    char config[CN_STORAGE_PATH_CAPACITY];
    char credential_path[CN_STORAGE_PATH_CAPACITY];
    char identity[CN_STORAGE_PATH_CAPACITY];
    const char *scenario = argv[2];

    if (verify_physical(argv, 3, &verified) != 0)
        return 1;
    if (strcmp(scenario, "disabled") != 0 &&
        strcmp(scenario, "ready") != 0 &&
        strcmp(scenario, "missing-identity") != 0)
        return 2;
    if (cn_storage_layout_init(&layout, verified.root, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(&layout, NULL) != CN_STORAGE_OK ||
        !stores(&layout, &settings, &credentials) ||
        !paths(&layout, config, credential_path, identity))
        return 1;
    if (access(config, F_OK) == 0 || access(credential_path, F_OK) == 0 ||
        access(identity, F_OK) == 0)
        return 1;
    if (!write_settings(&settings, !strcmp(scenario, "disabled") ? 0 : 1))
        return 1;
    if (!strcmp(scenario, "ready") || !strcmp(scenario, "missing-identity")) {
        if (!write_credentials(&credentials))
            return 1;
    }
    if (!strcmp(scenario, "ready") &&
        !identity_fixture(identity, TEST_ID, "1", 0))
        return 1;
    printf("PERSISTED SYNC PROFILE SETUP storage=verified scenario=%s "
           "phase=fixture-setup network=not-attempted\n", scenario);
    return 0;
}

static int physical_compose(char **argv)
{
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_persisted_sync_profile profile;
    cn_persisted_sync_profile_report report;

    if (verify_physical(argv, 2, &verified) != 0)
        return 1;
    if (cn_storage_layout_init(&layout, verified.root, NULL) != CN_STORAGE_OK)
        return 1;
    report = cn_persisted_sync_profile_load(&layout, &profile);
    printf("PERSISTED SYNC PROFILE GATE storage=verified result=%s "
           "phase=composition settings-attempted=%d settings-result=%s "
           "credentials-attempted=%d credentials-result=%s "
           "identity-attempted=%d identity-result=%s network=not-attempted\n",
           cn_persisted_sync_profile_status_name(report.status),
           report.settings_attempted,
           cn_settings_result_name(report.settings_result),
           report.credentials_attempted,
           cn_credential_result_name(report.credential_result),
           report.identity_attempted,
           cn_device_identity_result_name(report.identity_result));
    cn_persisted_sync_profile_clear(&profile);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--smoke"))
        return smoke();
    if (argc == 7 && !strcmp(argv[1], "--physical-setup"))
        return physical_setup(argv);
    if (argc == 6 && !strcmp(argv[1], "--physical-compose"))
        return physical_compose(argv);
    fprintf(stderr, "usage: persisted-sync-profile-test --smoke | "
                    "--physical-setup <disabled|ready|missing-identity> "
                    "<mount> <root> <major> <minor> | "
                    "--physical-compose <mount> <root> <major> <minor>\n");
    return 2;
}
