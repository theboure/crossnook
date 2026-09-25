#include <stdio.h>
#include <string.h>

#include "account/sync_activation.h"

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (length--)
        *p++ = 0;
}

static cn_sync_activation_report initial_report(void)
{
    cn_sync_activation_report report;
    memset(&report, 0, sizeof report);
    report.status = CN_SYNC_ACTIVATION_INVALID_ARGUMENT;
    report.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    report.credential_result = CN_CREDENTIAL_INVALID;
    report.identity_result = CN_DEVICE_ID_INVALID_ARGUMENT;
    report.time_result = CN_TIME_INVALID;
    report.dns_result = CN_DNS_INVALID;
    report.kosync_result = CN_KOSYNC_INVALID;
    report.kosync_outcome.transport_result = CN_NETSIMPLE_INVALID;
    return report;
}

static int state_path(const cn_storage_layout *layout, char *output,
                      size_t capacity)
{
    char progress[CN_STORAGE_PATH_CAPACITY];
    size_t length;
    int written;

    if (!layout || !output || capacity == 0 ||
        cn_storage_layout_path(layout, CN_STORAGE_LOCATION_PROGRESS,
                               progress, sizeof progress) != CN_STORAGE_OK)
        return 0;
    length = strlen(progress);
    if (length <= sizeof "/progress" - 1 ||
        strcmp(progress + length - (sizeof "/progress" - 1),
               "/progress") != 0)
        return 0;
    progress[length - (sizeof "/progress" - 1)] = '\0';
    written = snprintf(output, capacity, "%s", progress);
    return written > 0 && (size_t)written < capacity;
}

static int valid_runtime(const cn_sync_activation_runtime *runtime)
{
    size_t i;

    if (!runtime || !runtime->dns || !runtime->tls ||
        !runtime->tls->ca_path || !runtime->tls->ca_path[0] ||
        runtime->dns->server_count == 0 ||
        runtime->dns->server_count > CN_DNS_MAX_SERVERS)
        return 0;
    for (i = 0; i < runtime->dns->server_count; ++i)
        if (!runtime->dns->servers[i] || !runtime->dns->servers[i][0])
            return 0;
    if (runtime->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
        runtime->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED)
        return 0;
    return runtime->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH ||
           (runtime->time && !runtime->tls->get_time);
}

static cn_sync_activation_status map_authorization(cn_kosync_result result)
{
    switch (result) {
    case CN_KOSYNC_AUTH_FAILED:
        return CN_SYNC_ACTIVATION_AUTH_REJECTED;
    case CN_KOSYNC_TRANSPORT_ERROR:
        return CN_SYNC_ACTIVATION_HTTPS_FAILED;
    case CN_KOSYNC_HTTP_ERROR:
    case CN_KOSYNC_BAD_JSON:
    case CN_KOSYNC_BAD_PROTOCOL:
        return CN_SYNC_ACTIVATION_SERVICE_FAILED;
    case CN_KOSYNC_NO_MEMORY:
        return CN_SYNC_ACTIVATION_NO_MEMORY;
    case CN_KOSYNC_INVALID:
        return CN_SYNC_ACTIVATION_CONFIG_FAILED;
    case CN_KOSYNC_OK:
        return CN_SYNC_ACTIVATION_OK;
    default:
        return CN_SYNC_ACTIVATION_SERVICE_FAILED;
    }
}

cn_sync_activation_report cn_sync_activate(
    const cn_storage_layout *prepared_layout,
    const cn_sync_activation_runtime *runtime)
{
    cn_sync_activation_report report = initial_report();
    cn_settings settings;
    cn_credentials credentials;
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_device_identity_store identity_store;
    cn_kosync_client client;
    cn_dns_answer answer;
    cn_time_sample sample;
    char config_path[CN_STORAGE_PATH_CAPACITY];
    char credentials_path[CN_STORAGE_PATH_CAPACITY];
    char device_id[CN_DEVICE_ID_TEXT_CAPACITY];
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    cn_device_identity_result identity_result;
    cn_kosync_result client_result;
    int client_initialized = 0;

    memset(&settings, 0, sizeof settings);
    memset(&credentials, 0, sizeof credentials);
    memset(&settings_store, 0, sizeof settings_store);
    memset(&credential_store, 0, sizeof credential_store);
    memset(&identity_store, 0, sizeof identity_store);
    memset(&client, 0, sizeof client);
    memset(&answer, 0, sizeof answer);
    memset(&sample, 0, sizeof sample);
    memset(config_path, 0, sizeof config_path);
    memset(credentials_path, 0, sizeof credentials_path);
    memset(device_id, 0, sizeof device_id);

    if (!prepared_layout ||
        cn_storage_layout_path(prepared_layout, CN_STORAGE_LOCATION_CONFIG,
                               config_path, sizeof config_path) !=
            CN_STORAGE_OK) {
        goto done;
    }
    report.settings_attempted = 1;
    settings_result = cn_settings_store_init(&settings_store, config_path,
                                             &report.system_errno);
    report.settings_result = settings_result;
    if (settings_result != CN_SETTINGS_OK) {
        report.status = CN_SYNC_ACTIVATION_SETTINGS_FAILED;
        goto done;
    }
    settings_result = cn_settings_load(&settings_store, &settings,
                                       &report.system_errno);
    report.settings_result = settings_result;
    if (settings_result != CN_SETTINGS_OK) {
        report.status = CN_SYNC_ACTIVATION_SETTINGS_FAILED;
        goto done;
    }
    if (settings.kosync_enabled) {
        report.status = CN_SYNC_ACTIVATION_ALREADY_ENABLED;
        goto done;
    }
    if (cn_settings_validate(&settings) != CN_SETTINGS_OK) {
        report.settings_result = CN_SETTINGS_INVALID_SETTINGS;
        report.status = CN_SYNC_ACTIVATION_SETTINGS_FAILED;
        goto done;
    }
    if (!settings.kosync_base_url[0]) {
        report.status = CN_SYNC_ACTIVATION_CONFIG_FAILED;
        goto done;
    }

    report.credentials_attempted = 1;
    if (!state_path(prepared_layout, credentials_path,
                    sizeof credentials_path)) {
        report.credential_result = CN_CREDENTIAL_PATH_TOO_LONG;
        report.status = CN_SYNC_ACTIVATION_CREDENTIALS_FAILED;
        goto done;
    }
    credential_result = cn_credential_store_init(
        &credential_store, credentials_path, &report.system_errno);
    report.credential_result = credential_result;
    if (credential_result != CN_CREDENTIAL_OK) {
        report.status = CN_SYNC_ACTIVATION_CREDENTIALS_FAILED;
        goto done;
    }
    credential_result = cn_credential_store_load(
        &credential_store, &credentials, &report.system_errno);
    report.credential_result = credential_result;
    if (credential_result != CN_CREDENTIAL_OK) {
        report.status = CN_SYNC_ACTIVATION_CREDENTIALS_FAILED;
        goto done;
    }

    report.identity_attempted = 1;
    identity_result = cn_device_identity_store_init(
        &identity_store, prepared_layout, NULL, NULL);
    report.identity_result = identity_result;
    if (identity_result != CN_DEVICE_ID_OK) {
        report.status = CN_SYNC_ACTIVATION_IDENTITY_FAILED;
        goto done;
    }
    identity_result = cn_device_identity_load(
        &identity_store, device_id, sizeof device_id, &report.system_errno);
    report.identity_result = identity_result;
    if (identity_result != CN_DEVICE_ID_OK) {
        report.status = CN_SYNC_ACTIVATION_IDENTITY_FAILED;
        goto done;
    }

    if (!valid_runtime(runtime)) {
        report.status = CN_SYNC_ACTIVATION_CONFIG_FAILED;
        goto done;
    }
    client_result = cn_kosync_client_init(&client, settings.kosync_base_url,
                                          credentials.username,
                                          credentials.userkey);
    client_initialized = 1;
    if (client_result != CN_KOSYNC_OK || !client.use_tls) {
        report.kosync_result = client_result;
        report.status = CN_SYNC_ACTIVATION_CONFIG_FAILED;
        goto done;
    }

    if (runtime->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH) {
        report.time_attempted = 1;
        report.time_result = cn_timesimple_sync(runtime->time, &sample);
        if (report.time_result != CN_TIME_OK) {
            report.status = CN_SYNC_ACTIVATION_TRUSTED_TIME_FAILED;
            goto done;
        }
    } else {
        report.time_result = CN_TIME_OK;
    }

    report.dns_attempted = 1;
    report.dns_result = cn_dnssimple_resolve_a(runtime->dns, client.host,
                                               &answer);
    if (report.dns_result != CN_DNS_OK) {
        report.status = CN_SYNC_ACTIVATION_DNS_FAILED;
        goto done;
    }
    if (answer.count == 0) {
        report.dns_result = CN_DNS_NO_ADDRESS;
        report.status = CN_SYNC_ACTIVATION_DNS_FAILED;
        goto done;
    }
    report.kosync_result = cn_kosync_client_set_tls(
        &client, runtime->tls, answer.ipv4[0]);
    if (report.kosync_result != CN_KOSYNC_OK) {
        report.status = CN_SYNC_ACTIVATION_CONFIG_FAILED;
        goto done;
    }

    report.remote_auth_attempted = 1;
    report.kosync_result = cn_kosync_authorize(&client,
                                               &report.kosync_outcome);
    report.status = map_authorization(report.kosync_result);
    if (report.kosync_result != CN_KOSYNC_OK)
        goto done;
    report.remote_auth_succeeded = 1;

    settings.kosync_enabled = 1;
    report.settings_save_attempted = 1;
    report.settings_result = cn_settings_save(&settings_store, &settings,
                                              &report.system_errno);
    if (report.settings_result == CN_SETTINGS_DURABILITY_UNCERTAIN) {
        report.settings_enable_visible_possible = 1;
        report.status = CN_SYNC_ACTIVATION_DURABILITY_UNCERTAIN;
        goto done;
    }
    if (report.settings_result != CN_SETTINGS_OK) {
        report.status = CN_SYNC_ACTIVATION_SETTINGS_SAVE_FAILED;
        goto done;
    }
    report.settings_enable_visible_possible = 1;
    report.status = CN_SYNC_ACTIVATION_OK;

done:
    cn_credentials_clear(&credentials);
    clear_bytes(&settings, sizeof settings);
    clear_bytes(&settings_store, sizeof settings_store);
    clear_bytes(&credential_store, sizeof credential_store);
    clear_bytes(&identity_store, sizeof identity_store);
    if (client_initialized || client.use_tls)
        clear_bytes(&client, sizeof client);
    clear_bytes(device_id, sizeof device_id);
    clear_bytes(credentials_path, sizeof credentials_path);
    clear_bytes(config_path, sizeof config_path);
    return report;
}

const char *cn_sync_activation_status_name(cn_sync_activation_status status)
{
    static const char *const names[CN_SYNC_ACTIVATION_STATUS_COUNT] = {
        "ok", "already-enabled", "invalid-argument", "settings-failed",
        "credentials-failed", "identity-failed", "config-failed",
        "trusted-time-failed", "dns-failed", "auth-rejected",
        "https-failed", "service-failed", "settings-save-failed",
        "durability-uncertain", "no-memory"
    };
    return status >= 0 && status < CN_SYNC_ACTIVATION_STATUS_COUNT
               ? names[status] : "unknown";
}
