#include <stdio.h>
#include <string.h>

#include "account/account_bootstrap.h"

#define STATE_SUFFIX "/state"
#define PROGRESS_SUFFIX "/progress"

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)data;
    while (length--)
        *bytes++ = 0;
}

static int copy_bounded(char *output, size_t capacity, const char *input)
{
    size_t length;
    if (!output || !input)
        return 0;
    for (length = 0; length < capacity; ++length)
        if (input[length] == '\0')
            break;
    if (length == 0 || length >= capacity)
        return 0;
    memcpy(output, input, length + 1);
    return 1;
}

static int credential_text_valid(const char *text)
{
    size_t i, length;
    if (!text)
        return 0;
    for (length = 0; length <= CN_CREDENTIAL_USERNAME_MAX; ++length)
        if (text[length] == '\0')
            break;
    if (length == 0 || length > CN_CREDENTIAL_USERNAME_MAX)
        return 0;
    for (i = 0; i < length; ++i)
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] == 0x7f)
            return 0;
    return 1;
}

static int settings_equal(const cn_settings *left, const cn_settings *right)
{
    return left && right && left->kosync_enabled == right->kosync_enabled &&
           !strcmp(left->kosync_base_url, right->kosync_base_url) &&
           !strcmp(left->kosync_device_name, right->kosync_device_name);
}

static int credentials_equal(const cn_credentials *left,
                             const cn_credentials *right)
{
    return left && right && !strcmp(left->username, right->username) &&
           !strcmp(left->userkey, right->userkey);
}

static cn_account_bootstrap_report initial_report(void)
{
    cn_account_bootstrap_report report;
    memset(&report, 0, sizeof report);
    report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT;
    report.stage = CN_ACCOUNT_BOOTSTRAP_STAGE_NONE;
    report.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    report.credential_result = CN_CREDENTIAL_INVALID;
    report.userkey_result = CN_KOSYNC_USERKEY_INVALID_ARGUMENT;
    return report;
}

cn_account_bootstrap_report cn_account_bootstrap(
    const cn_storage_layout *layout, const char *base_url,
    const char *device_name, const char *username,
    const unsigned char *password, size_t password_length, int *system_errno)
{
    cn_account_bootstrap_report report = initial_report();
    cn_settings desired_settings, existing_settings;
    cn_credentials desired_credentials, existing_credentials;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    char config_directory[CN_STORAGE_PATH_CAPACITY];
    char progress_directory[CN_STORAGE_PATH_CAPACITY];
    char state_directory[CN_STORAGE_PATH_CAPACITY];
    char userkey[CN_ACCOUNT_USERKEY_TEXT_CAPACITY];
    size_t progress_length, state_length;
    int settings_missing, credentials_missing;
    int save_settings, save_credentials;
    cn_storage_result storage_result;

    if (system_errno)
        *system_errno = 0;
    memset(&desired_settings, 0, sizeof desired_settings);
    memset(&existing_settings, 0, sizeof existing_settings);
    memset(&desired_credentials, 0, sizeof desired_credentials);
    memset(&existing_credentials, 0, sizeof existing_credentials);
    memset(&settings_store, 0, sizeof settings_store);
    memset(&credential_store, 0, sizeof credential_store);
    memset(userkey, 0, sizeof userkey);
    report.stage = CN_ACCOUNT_BOOTSTRAP_STAGE_PREFLIGHT;

    if (!layout)
        goto done;
    if (!base_url || !device_name || !username || !password) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT;
        goto done;
    }
    if (password_length == 0 || password_length > CN_ACCOUNT_PASSWORD_MAX ||
        !credential_text_valid(username)) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT;
        goto done;
    }
    cn_settings_defaults(&desired_settings);
    if (!copy_bounded(desired_settings.kosync_base_url,
                      sizeof desired_settings.kosync_base_url, base_url) ||
        !copy_bounded(desired_settings.kosync_device_name,
                      sizeof desired_settings.kosync_device_name, device_name)) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT;
        goto done;
    }
    desired_settings.kosync_enabled = 0;
    report.settings_result = cn_settings_validate(&desired_settings);
    if (report.settings_result != CN_SETTINGS_OK || !base_url[0]) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT;
        goto done;
    }
    report.userkey_result = cn_kosync_userkey_from_password(
        password, password_length, userkey, sizeof userkey);
    if (report.userkey_result != CN_KOSYNC_USERKEY_OK) {
        report.result = CN_ACCOUNT_BOOTSTRAP_DERIVATION_FAILED;
        goto done;
    }
    memcpy(desired_credentials.username, username, strlen(username) + 1);
    memcpy(desired_credentials.userkey, userkey, sizeof userkey);

    storage_result = cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG,
                                             config_directory,
                                             sizeof config_directory);
    if (storage_result != CN_STORAGE_OK) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT;
        goto done;
    }
    storage_result = cn_storage_layout_path(layout, CN_STORAGE_LOCATION_PROGRESS,
                                             progress_directory,
                                             sizeof progress_directory);
    if (storage_result != CN_STORAGE_OK) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT;
        goto done;
    }
    progress_length = strlen(progress_directory);
    if (progress_length <= sizeof PROGRESS_SUFFIX - 1 ||
        strcmp(progress_directory + progress_length -
                   (sizeof PROGRESS_SUFFIX - 1), PROGRESS_SUFFIX) != 0) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT;
        goto done;
    }
    state_length = progress_length - (sizeof PROGRESS_SUFFIX - 1);
    if (state_length + sizeof STATE_SUFFIX > sizeof state_directory) {
        report.result = CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT;
        goto done;
    }
    memcpy(state_directory, progress_directory, state_length);
    state_directory[state_length] = '\0';

    report.settings_result = cn_settings_store_init(
        &settings_store, config_directory, system_errno);
    if (report.settings_result != CN_SETTINGS_OK) {
        report.result = CN_ACCOUNT_BOOTSTRAP_EXISTING_SETTINGS_FAILED;
        goto done;
    }
    report.credential_result = cn_credential_store_init(
        &credential_store, state_directory, system_errno);
    if (report.credential_result != CN_CREDENTIAL_OK) {
        report.result = CN_ACCOUNT_BOOTSTRAP_EXISTING_CREDENTIALS_FAILED;
        goto done;
    }

    report.settings_result = cn_settings_load(&settings_store,
                                              &existing_settings, system_errno);
    settings_missing = report.settings_result == CN_SETTINGS_MISSING;
    if (report.settings_result != CN_SETTINGS_OK && !settings_missing) {
        report.result = CN_ACCOUNT_BOOTSTRAP_EXISTING_SETTINGS_FAILED;
        goto done;
    }
    if (!settings_missing && existing_settings.kosync_enabled) {
        report.result = CN_ACCOUNT_BOOTSTRAP_EXISTING_ACTIVE;
        goto done;
    }
    report.credential_result = cn_credential_store_load(
        &credential_store, &existing_credentials, system_errno);
    credentials_missing = report.credential_result == CN_CREDENTIAL_MISSING;
    if (report.credential_result != CN_CREDENTIAL_OK && !credentials_missing) {
        report.result = CN_ACCOUNT_BOOTSTRAP_EXISTING_CREDENTIALS_FAILED;
        goto done;
    }

    save_settings = settings_missing ||
                    !settings_equal(&existing_settings, &desired_settings);
    save_credentials = credentials_missing ||
                       !credentials_equal(&existing_credentials,
                                          &desired_credentials);
    if (!save_settings && !save_credentials) {
        report.result = CN_ACCOUNT_BOOTSTRAP_OK;
        goto done;
    }
    if (save_settings) {
        report.stage = CN_ACCOUNT_BOOTSTRAP_STAGE_SETTINGS;
        report.settings_result = cn_settings_save(&settings_store,
                                                  &desired_settings,
                                                  system_errno);
        if (report.settings_result == CN_SETTINGS_OK ||
            report.settings_result == CN_SETTINGS_DURABILITY_UNCERTAIN)
            report.settings_written = 1;
        if (report.settings_result == CN_SETTINGS_DURABILITY_UNCERTAIN) {
            report.result = CN_ACCOUNT_BOOTSTRAP_DURABILITY_UNCERTAIN;
            goto done;
        }
        if (report.settings_result != CN_SETTINGS_OK) {
            report.result = CN_ACCOUNT_BOOTSTRAP_SETTINGS_WRITE_FAILED;
            goto done;
        }
    }
    if (save_credentials) {
        report.stage = CN_ACCOUNT_BOOTSTRAP_STAGE_CREDENTIALS;
        report.credential_result = cn_credential_store_save(
            &credential_store, &desired_credentials, system_errno);
        if (report.credential_result == CN_CREDENTIAL_OK ||
            report.credential_result == CN_CREDENTIAL_DURABILITY_UNCERTAIN)
            report.credentials_written = 1;
        if (report.credential_result == CN_CREDENTIAL_DURABILITY_UNCERTAIN) {
            report.result = CN_ACCOUNT_BOOTSTRAP_DURABILITY_UNCERTAIN;
            goto done;
        }
        if (report.credential_result != CN_CREDENTIAL_OK) {
            report.result = CN_ACCOUNT_BOOTSTRAP_CREDENTIALS_WRITE_FAILED;
            goto done;
        }
    }
    report.result = CN_ACCOUNT_BOOTSTRAP_OK;

done:
    clear_bytes(userkey, sizeof userkey);
    cn_credentials_clear(&desired_credentials);
    cn_credentials_clear(&existing_credentials);
    clear_bytes(&settings_store, sizeof settings_store);
    clear_bytes(&credential_store, sizeof credential_store);
    return report;
}

const char *cn_account_bootstrap_result_name(cn_account_bootstrap_result result)
{
    static const char *const names[CN_ACCOUNT_BOOTSTRAP_RESULT_COUNT] = {
        "ok", "invalid-argument", "invalid-input", "existing-active",
        "existing-settings-failed", "existing-credentials-failed",
        "derivation-failed", "settings-write-failed",
        "credentials-write-failed", "durability-uncertain"
    };
    return result >= 0 && result < CN_ACCOUNT_BOOTSTRAP_RESULT_COUNT
               ? names[result] : "unknown";
}

const char *cn_account_bootstrap_stage_name(cn_account_bootstrap_stage stage)
{
    static const char *const names[CN_ACCOUNT_BOOTSTRAP_STAGE_CREDENTIALS + 1] = {
        "none", "preflight", "settings", "credentials"
    };
    return stage >= 0 && stage <= CN_ACCOUNT_BOOTSTRAP_STAGE_CREDENTIALS
               ? names[stage] : "unknown";
}
