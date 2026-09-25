/* Separate explicit remote-wins controller; published normal sync untouched. */
#include <string.h>

#include "sync/sync_controller_internal.h"

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (length--) *p++ = 0;
}

void cn_sync_pull_result_init_internal(cn_sync_pull_result *result)
{
    if (!result) return;
    memset(result, 0, sizeof *result);
    result->stage = CN_SYNC_PULL_INVALID;
    result->outcome = CN_SYNC_PULL_INTERNAL_FAILURE;
    result->settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    result->credential_result = CN_CREDENTIAL_INVALID;
    cn_kosync_pull_result_init(&result->pull);
}

static cn_sync_pull_outcome dns_failure(cn_dns_result code)
{
    switch (code) {
    case CN_DNS_SOCKET_FAILED:
    case CN_DNS_NETWORK_FAILED:
    case CN_DNS_TIMEOUT:
    case CN_DNS_SERVER_FAILURE:
        return CN_SYNC_PULL_CONNECTIVITY_FAILURE;
    case CN_DNS_INVALID:
    case CN_DNS_INVALID_HOSTNAME:
        return CN_SYNC_PULL_CONFIGURATION_FAILURE;
    case CN_DNS_ENTROPY_FAILED:
        return CN_SYNC_PULL_INTERNAL_FAILURE;
    default:
        return CN_SYNC_PULL_SERVICE_FAILURE;
    }
}

static cn_sync_pull_outcome transport_failure(cn_netsimple_result code)
{
    switch (code) {
    case CN_NETSIMPLE_RESOLVE_ERROR:
    case CN_NETSIMPLE_CONNECT_REFUSED:
    case CN_NETSIMPLE_NETWORK_UNREACHABLE:
    case CN_NETSIMPLE_HOST_UNREACHABLE:
    case CN_NETSIMPLE_CONNECT_TIMEOUT:
    case CN_NETSIMPLE_CONNECT_ERROR:
    case CN_NETSIMPLE_SEND_ERROR:
    case CN_NETSIMPLE_RECV_ERROR:
    case CN_NETSIMPLE_RECV_TIMEOUT:
    case CN_NETSIMPLE_SEND_TIMEOUT:
    case CN_NETSIMPLE_TLS_HANDSHAKE_TIMEOUT:
    case CN_NETSIMPLE_TLS_RECV_TIMEOUT:
        return CN_SYNC_PULL_CONNECTIVITY_FAILURE;
    case CN_NETSIMPLE_TLS_INVALID_CA:
    case CN_NETSIMPLE_TLS_TRUST_FAILED:
    case CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH:
    case CN_NETSIMPLE_TLS_CERT_TIME_FAILED:
    case CN_NETSIMPLE_TLS_CERT_INVALID:
        return CN_SYNC_PULL_SECURITY_FAILURE;
    case CN_NETSIMPLE_INVALID:
        return CN_SYNC_PULL_CONFIGURATION_FAILURE;
    case CN_NETSIMPLE_TLS_ENTROPY_FAILED:
    case CN_NETSIMPLE_TLS_INTERNAL:
        return CN_SYNC_PULL_INTERNAL_FAILURE;
    default:
        return CN_SYNC_PULL_SERVICE_FAILURE;
    }
}

static cn_sync_pull_outcome classify_pull(const cn_kosync_pull_result *pull)
{
    switch (pull->status) {
    case CN_KOSYNC_PULL_REMOTE_MISSING:
        return CN_SYNC_PULL_REMOTE_MISSING_OUTCOME;
    case CN_KOSYNC_PULL_PERSISTED:
        return CN_SYNC_PULL_PERSISTED_OUTCOME;
    case CN_KOSYNC_PULL_AUTH_FAILED:
        return CN_SYNC_PULL_AUTH_REQUIRED;
    case CN_KOSYNC_PULL_TRUSTED_TIME_FAILED:
        return CN_SYNC_PULL_TRUSTED_TIME_UNAVAILABLE;
    case CN_KOSYNC_PULL_DNS_FAILED:
        return dns_failure(pull->dns_result);
    case CN_KOSYNC_PULL_HTTPS_FAILED:
        return transport_failure(pull->kosync_outcome.transport_result);
    case CN_KOSYNC_PULL_PROTOCOL_FAILED:
        return CN_SYNC_PULL_SERVICE_FAILURE;
    case CN_KOSYNC_PULL_LOCAL_FAILED:
    case CN_KOSYNC_PULL_IDENTITY_FAILED:
        return CN_SYNC_PULL_LOCAL_FAILURE;
    case CN_KOSYNC_PULL_INVALID:
        return CN_SYNC_PULL_CONFIGURATION_FAILURE;
    default:
        return CN_SYNC_PULL_INTERNAL_FAILURE;
    }
}

void cn_sync_pull_remote_current_book_resolved(
    const cn_sync_resolved_config *config,
    cn_sync_pull_result *output)
{
    cn_kosync_client client;
    cn_kosync_pull_config pull_config;
    cn_kosync_result client_result;

    if (!output) return;
    if (!config || !config->progress_store || !config->document_path ||
        !config->document_path[0] || !config->dns || !config->tls ||
        !config->tls->ca_path ||
        (config->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         config->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED) ||
        (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         (!config->time || config->tls->get_time))) {
        output->stage = CN_SYNC_PULL_CONFIG_FAILED;
        output->outcome = CN_SYNC_PULL_CONFIGURATION_FAILURE;
        return;
    }
    memset(&client, 0, sizeof client);
    client_result = cn_kosync_client_init(&client, config->base_url,
                                          config->username, config->userkey);
    if (client_result != CN_KOSYNC_OK || !client.use_tls) {
        output->stage = CN_SYNC_PULL_CONFIG_FAILED;
        output->outcome = CN_SYNC_PULL_CONFIGURATION_FAILURE;
        clear_bytes(&client, sizeof client);
        return;
    }
    /* Pull does not consume or transmit config->device_id. */
    memset(&pull_config, 0, sizeof pull_config);
    pull_config.document_path = config->document_path;
    pull_config.progress_store = config->progress_store;
    pull_config.time_policy = config->time_policy;
    pull_config.time = config->time;
    pull_config.dns = config->dns;
    pull_config.tls = config->tls;
    pull_config.client = &client;
    (void)cn_kosync_pull_remote_once(&pull_config, &output->pull);
    output->stage = CN_SYNC_PULL_EXECUTED;
    output->outcome = classify_pull(&output->pull);
    output->local_mutation = output->pull.local_saved
                                ? CN_KOSYNC_MUTATION_CONFIRMED
                                : output->pull.local_save_attempted
                                      ? CN_KOSYNC_MUTATION_POSSIBLE
                                      : CN_KOSYNC_MUTATION_NONE;
    clear_bytes(&client, sizeof client);
}

cn_sync_pull_result cn_sync_pull_remote_current_book(
    const cn_sync_controller_config *config)
{
    cn_sync_pull_result output;
    cn_settings settings;
    cn_credentials credentials;
    cn_sync_resolved_config resolved;

    cn_sync_pull_result_init_internal(&output);
    if (!config || !config->settings_store) return output;
    output.settings_result = cn_settings_load(config->settings_store,
                                               &settings, NULL);
    if (output.settings_result != CN_SETTINGS_OK &&
        output.settings_result != CN_SETTINGS_MISSING) {
        output.stage = CN_SYNC_PULL_SETTINGS_FAILED;
        output.outcome = CN_SYNC_PULL_CONFIGURATION_FAILURE;
        return output;
    }
    if (output.settings_result == CN_SETTINGS_MISSING ||
        !settings.kosync_enabled) {
        output.stage = CN_SYNC_PULL_DISABLED;
        output.outcome = CN_SYNC_PULL_DISABLED_OUTCOME;
        return output;
    }
    if (!config->credential_store) {
        output.stage = CN_SYNC_PULL_CONFIG_FAILED;
        output.outcome = CN_SYNC_PULL_CONFIGURATION_FAILURE;
        return output;
    }
    memset(&credentials, 0, sizeof credentials);
    output.credential_result = cn_credential_store_load(
        config->credential_store, &credentials, NULL);
    if (output.credential_result != CN_CREDENTIAL_OK) {
        output.stage = CN_SYNC_PULL_CREDENTIALS_FAILED;
        output.outcome = output.credential_result == CN_CREDENTIAL_MISSING
                             ? CN_SYNC_PULL_AUTH_REQUIRED
                             : output.credential_result == CN_CREDENTIAL_CORRUPT ||
                               output.credential_result == CN_CREDENTIAL_UNSUPPORTED_VERSION ||
                               output.credential_result == CN_CREDENTIAL_INVALID
                                   ? CN_SYNC_PULL_CONFIGURATION_FAILURE
                                   : CN_SYNC_PULL_LOCAL_FAILURE;
        cn_credentials_clear(&credentials);
        return output;
    }
    if (cn_settings_validate(&settings) != CN_SETTINGS_OK) {
        output.stage = CN_SYNC_PULL_CONFIG_FAILED;
        output.outcome = CN_SYNC_PULL_CONFIGURATION_FAILURE;
    } else {
        memset(&resolved, 0, sizeof resolved);
        resolved.base_url = settings.kosync_base_url;
        resolved.device_name = settings.kosync_device_name;
        resolved.username = credentials.username;
        resolved.userkey = credentials.userkey;
        resolved.device_id = config->device_id;
        resolved.progress_store = config->progress_store;
        resolved.document_path = config->document_path;
        resolved.dns = config->dns;
        resolved.tls = config->tls;
        resolved.time_policy = config->time_policy;
        resolved.time = config->time;
        cn_sync_pull_remote_current_book_resolved(&resolved, &output);
    }
    cn_credentials_clear(&credentials);
    return output;
}

const char *cn_sync_pull_stage_name(cn_sync_pull_stage stage)
{
    static const char *const names[] = {
        "invalid", "disabled", "settings-failed", "credentials-failed",
        "config-failed", "executed"
    };
    return stage >= CN_SYNC_PULL_INVALID && stage <= CN_SYNC_PULL_EXECUTED
               ? names[stage] : "unknown";
}

const char *cn_sync_pull_outcome_name(cn_sync_pull_outcome outcome)
{
    static const char *const names[CN_SYNC_PULL_OUTCOME_COUNT] = {
        "internal-failure", "disabled", "remote-missing", "persisted",
        "auth-required", "trusted-time-unavailable", "connectivity-failure",
        "security-failure", "service-failure", "local-failure",
        "configuration-failure"
    };
    return outcome >= 0 && outcome < CN_SYNC_PULL_OUTCOME_COUNT
               ? names[outcome] : "unknown";
}
