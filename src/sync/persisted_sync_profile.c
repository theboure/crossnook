#include <stdio.h>
#include <string.h>

#include "sync/persisted_sync_profile.h"

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (length--)
        *p++ = 0;
}

static cn_persisted_sync_profile_report initial_report(void)
{
    cn_persisted_sync_profile_report report;
    memset(&report, 0, sizeof report);
    report.status = CN_PERSISTED_SYNC_PROFILE_INVALID_ARGUMENT;
    report.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    report.credential_result = CN_CREDENTIAL_INVALID;
    report.identity_result = CN_DEVICE_ID_INVALID_ARGUMENT;
    return report;
}

static int bounded_text(const char *text, size_t capacity, int require_value)
{
    size_t length;
    if (!text)
        return 0;
    for (length = 0; length < capacity; ++length)
        if (text[length] == '\0')
            return !require_value || length != 0;
    return 0;
}

static int state_path(const cn_storage_layout *layout, char *output,
                      size_t capacity)
{
    int written;
    if (!layout || !output || capacity == 0)
        return 0;
    written = snprintf(output, capacity, "%s/state", layout->root);
    return written > 0 && (size_t)written < capacity;
}

void cn_persisted_sync_profile_clear(cn_persisted_sync_profile *profile)
{
    if (profile)
        clear_bytes(profile, sizeof *profile);
}

cn_persisted_sync_profile_report cn_persisted_sync_profile_load(
    const cn_storage_layout *prepared_layout,
    cn_persisted_sync_profile *output)
{
    cn_persisted_sync_profile_report report = initial_report();
    cn_settings settings;
    cn_credentials credentials;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_device_identity_store identity_store;
    char config_path[CN_STORAGE_PATH_CAPACITY];
    char state_path_buffer[CN_STORAGE_PATH_CAPACITY];
    char device_id[CN_DEVICE_ID_TEXT_CAPACITY];
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    cn_device_identity_result identity_result;

    if (output)
        cn_persisted_sync_profile_clear(output);
    if (!prepared_layout || !output)
        return report;
    memset(&settings, 0, sizeof settings);
    memset(&credentials, 0, sizeof credentials);
    memset(&settings_store, 0, sizeof settings_store);
    memset(&credential_store, 0, sizeof credential_store);
    memset(&identity_store, 0, sizeof identity_store);
    memset(device_id, 0, sizeof device_id);

    report.settings_attempted = 1;
    if (cn_storage_layout_path(prepared_layout, CN_STORAGE_LOCATION_CONFIG,
                               config_path, sizeof config_path) != CN_STORAGE_OK) {
        report.status = CN_PERSISTED_SYNC_PROFILE_SETTINGS_FAILED;
        report.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
        goto done;
    }
    settings_result = cn_settings_store_init(&settings_store, config_path,
                                             &report.system_errno);
    report.settings_result = settings_result;
    if (settings_result != CN_SETTINGS_OK) {
        report.status = CN_PERSISTED_SYNC_PROFILE_SETTINGS_FAILED;
        goto done;
    }
    settings_result = cn_settings_load(&settings_store, &settings,
                                       &report.system_errno);
    report.settings_result = settings_result;
    if (settings_result == CN_SETTINGS_MISSING ||
        (settings_result == CN_SETTINGS_OK && !settings.kosync_enabled)) {
        report.status = CN_PERSISTED_SYNC_PROFILE_DISABLED;
        goto done;
    }
    if (settings_result != CN_SETTINGS_OK) {
        report.status = CN_PERSISTED_SYNC_PROFILE_SETTINGS_FAILED;
        goto done;
    }

    report.credentials_attempted = 1;
    if (!state_path(prepared_layout, state_path_buffer,
                    sizeof state_path_buffer)) {
        report.credential_result = CN_CREDENTIAL_PATH_TOO_LONG;
        report.status = CN_PERSISTED_SYNC_PROFILE_CREDENTIALS_FAILED;
        goto done;
    }
    credential_result = cn_credential_store_init(
        &credential_store, state_path_buffer, &report.system_errno);
    report.credential_result = credential_result;
    if (credential_result != CN_CREDENTIAL_OK) {
        report.status = CN_PERSISTED_SYNC_PROFILE_CREDENTIALS_FAILED;
        goto done;
    }
    credential_result = cn_credential_store_load(
        &credential_store, &credentials, &report.system_errno);
    report.credential_result = credential_result;
    if (credential_result != CN_CREDENTIAL_OK) {
        report.status = CN_PERSISTED_SYNC_PROFILE_CREDENTIALS_FAILED;
        goto done;
    }

    report.identity_attempted = 1;
    identity_result = cn_device_identity_store_init(
        &identity_store, prepared_layout, NULL, NULL);
    report.identity_result = identity_result;
    if (identity_result != CN_DEVICE_ID_OK) {
        report.status = CN_PERSISTED_SYNC_PROFILE_IDENTITY_FAILED;
        goto done;
    }
    identity_result = cn_device_identity_load(
        &identity_store, device_id, sizeof device_id, &report.system_errno);
    report.identity_result = identity_result;
    if (identity_result != CN_DEVICE_ID_OK) {
        report.status = CN_PERSISTED_SYNC_PROFILE_IDENTITY_FAILED;
        goto done;
    }

    memcpy(output->base_url, settings.kosync_base_url,
           sizeof output->base_url);
    memcpy(output->device_name, settings.kosync_device_name,
           sizeof output->device_name);
    memcpy(output->username, credentials.username, sizeof output->username);
    memcpy(output->userkey, credentials.userkey, sizeof output->userkey);
    memcpy(output->device_id, device_id, sizeof output->device_id);
    if (!bounded_text(output->base_url, sizeof output->base_url, 1) ||
        !bounded_text(output->username, sizeof output->username, 1) ||
        !bounded_text(output->userkey, sizeof output->userkey, 1) ||
        !bounded_text(output->device_id, sizeof output->device_id, 1) ||
        !bounded_text(output->device_name, sizeof output->device_name, 1)) {
        report.status = CN_PERSISTED_SYNC_PROFILE_VALIDATION_FAILED;
        goto done;
    }
    report.status = CN_PERSISTED_SYNC_PROFILE_READY;

done:
    cn_credentials_clear(&credentials);
    clear_bytes(&settings, sizeof settings);
    clear_bytes(&settings_store, sizeof settings_store);
    clear_bytes(&credential_store, sizeof credential_store);
    clear_bytes(&identity_store, sizeof identity_store);
    clear_bytes(device_id, sizeof device_id);
    if (report.status != CN_PERSISTED_SYNC_PROFILE_READY)
        cn_persisted_sync_profile_clear(output);
    return report;
}

const char *cn_persisted_sync_profile_status_name(
    cn_persisted_sync_profile_status status)
{
    static const char *const names[CN_PERSISTED_SYNC_PROFILE_STATUS_COUNT] = {
        "invalid-argument", "disabled", "ready", "settings-failed",
        "credentials-failed", "identity-failed", "validation-failed"
    };
    return status >= 0 && status < CN_PERSISTED_SYNC_PROFILE_STATUS_COUNT
               ? names[status] : "unknown";
}
