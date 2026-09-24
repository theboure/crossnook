/* Explicit persisted-local-wins controller; published normal sync untouched. */
#include <string.h>

#include "sync/sync_controller.h"

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (length--) *p++ = 0;
}

static cn_sync_push_result initial_result(void)
{
    cn_sync_push_result result;
    memset(&result, 0, sizeof result);
    result.stage = CN_SYNC_PUSH_INVALID;
    result.outcome = CN_SYNC_PUSH_INTERNAL_FAILURE;
    result.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    result.credential_result = CN_CREDENTIAL_INVALID;
    cn_kosync_push_result_init(&result.push);
    return result;
}

static cn_sync_push_outcome dns_failure(cn_dns_result code)
{
    switch (code) {
    case CN_DNS_SOCKET_FAILED:
    case CN_DNS_NETWORK_FAILED:
    case CN_DNS_TIMEOUT:
    case CN_DNS_SERVER_FAILURE:
        return CN_SYNC_PUSH_CONNECTIVITY_FAILURE;
    case CN_DNS_INVALID:
    case CN_DNS_INVALID_HOSTNAME:
        return CN_SYNC_PUSH_CONFIGURATION_FAILURE;
    case CN_DNS_ENTROPY_FAILED:
        return CN_SYNC_PUSH_INTERNAL_FAILURE;
    default:
        return CN_SYNC_PUSH_SERVICE_FAILURE;
    }
}

static cn_sync_push_outcome transport_failure(cn_netsimple_result code)
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
        return CN_SYNC_PUSH_CONNECTIVITY_FAILURE;
    case CN_NETSIMPLE_TLS_INVALID_CA:
    case CN_NETSIMPLE_TLS_TRUST_FAILED:
    case CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH:
    case CN_NETSIMPLE_TLS_CERT_TIME_FAILED:
    case CN_NETSIMPLE_TLS_CERT_INVALID:
        return CN_SYNC_PUSH_SECURITY_FAILURE;
    case CN_NETSIMPLE_INVALID:
        return CN_SYNC_PUSH_CONFIGURATION_FAILURE;
    case CN_NETSIMPLE_TLS_ENTROPY_FAILED:
    case CN_NETSIMPLE_TLS_INTERNAL:
        return CN_SYNC_PUSH_INTERNAL_FAILURE;
    default:
        return CN_SYNC_PUSH_SERVICE_FAILURE;
    }
}

static cn_sync_push_outcome classify_push(const cn_kosync_push_result *push)
{
    switch (push->status) {
    case CN_KOSYNC_PUSH_UPLOADED: return CN_SYNC_PUSH_UPLOADED_OUTCOME;
    case CN_KOSYNC_PUSH_LOCAL_MISSING: return CN_SYNC_PUSH_LOCAL_MISSING_OUTCOME;
    case CN_KOSYNC_PUSH_LOCAL_UNSUPPORTED:
        return CN_SYNC_PUSH_LOCAL_UNSUPPORTED_OUTCOME;
    case CN_KOSYNC_PUSH_LOCAL_FAILED:
    case CN_KOSYNC_PUSH_IDENTITY_FAILED:
        return CN_SYNC_PUSH_LOCAL_FAILURE;
    case CN_KOSYNC_PUSH_AUTH_FAILED: return CN_SYNC_PUSH_AUTH_REQUIRED;
    case CN_KOSYNC_PUSH_TRUSTED_TIME_FAILED:
        return CN_SYNC_PUSH_TRUSTED_TIME_UNAVAILABLE;
    case CN_KOSYNC_PUSH_DNS_FAILED: return dns_failure(push->dns_result);
    case CN_KOSYNC_PUSH_HTTPS_FAILED:
        return transport_failure(push->kosync_outcome.transport_result);
    case CN_KOSYNC_PUSH_PROTOCOL_FAILED: return CN_SYNC_PUSH_SERVICE_FAILURE;
    case CN_KOSYNC_PUSH_INVALID: return CN_SYNC_PUSH_CONFIGURATION_FAILURE;
    default: return CN_SYNC_PUSH_INTERNAL_FAILURE;
    }
}

cn_sync_push_result cn_sync_push_local_current_book(
    const cn_sync_controller_config *config)
{
    cn_sync_push_result output = initial_result();
    cn_settings settings;
    cn_credentials credentials;
    cn_kosync_client client;
    cn_kosync_push_config push_config;
    cn_kosync_result client_result;

    if (!config || !config->settings_store) return output;
    output.settings_result = cn_settings_load(config->settings_store,
                                               &settings, NULL);
    if (output.settings_result != CN_SETTINGS_OK &&
        output.settings_result != CN_SETTINGS_MISSING) {
        output.stage = CN_SYNC_PUSH_SETTINGS_FAILED;
        output.outcome = CN_SYNC_PUSH_CONFIGURATION_FAILURE;
        return output;
    }
    if (output.settings_result == CN_SETTINGS_MISSING ||
        !settings.kosync_enabled) {
        output.stage = CN_SYNC_PUSH_DISABLED;
        output.outcome = CN_SYNC_PUSH_DISABLED_OUTCOME;
        return output;
    }
    if (!config->credential_store) {
        output.stage = CN_SYNC_PUSH_CONFIG_FAILED;
        output.outcome = CN_SYNC_PUSH_CONFIGURATION_FAILURE;
        return output;
    }
    memset(&credentials, 0, sizeof credentials);
    memset(&client, 0, sizeof client);
    output.credential_result = cn_credential_store_load(
        config->credential_store, &credentials, NULL);
    if (output.credential_result != CN_CREDENTIAL_OK) {
        output.stage = CN_SYNC_PUSH_CREDENTIALS_FAILED;
        output.outcome = output.credential_result == CN_CREDENTIAL_MISSING
                             ? CN_SYNC_PUSH_AUTH_REQUIRED
                             : output.credential_result == CN_CREDENTIAL_CORRUPT ||
                               output.credential_result == CN_CREDENTIAL_UNSUPPORTED_VERSION ||
                               output.credential_result == CN_CREDENTIAL_INVALID
                                   ? CN_SYNC_PUSH_CONFIGURATION_FAILURE
                                   : CN_SYNC_PUSH_LOCAL_FAILURE;
        goto done;
    }
    if (cn_settings_validate(&settings) != CN_SETTINGS_OK ||
        !config->progress_store || !config->document_path ||
        !config->document_path[0] || !config->device_id ||
        !config->dns || !config->tls || !config->tls->ca_path ||
        (config->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         config->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED) ||
        (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         (!config->time || config->tls->get_time))) {
        output.stage = CN_SYNC_PUSH_CONFIG_FAILED;
        output.outcome = CN_SYNC_PUSH_CONFIGURATION_FAILURE;
        goto done;
    }
    client_result = cn_kosync_client_init(&client, settings.kosync_base_url,
                                           credentials.username,
                                           credentials.userkey);
    if (client_result != CN_KOSYNC_OK || !client.use_tls) {
        output.stage = CN_SYNC_PUSH_CONFIG_FAILED;
        output.outcome = CN_SYNC_PUSH_CONFIGURATION_FAILURE;
        goto done;
    }
    memset(&push_config, 0, sizeof push_config);
    push_config.document_path = config->document_path;
    push_config.progress_store = config->progress_store;
    push_config.time_policy = config->time_policy;
    push_config.time = config->time;
    push_config.dns = config->dns;
    push_config.tls = config->tls;
    push_config.client = &client;
    push_config.device = settings.kosync_device_name;
    push_config.device_id = config->device_id;
    (void)cn_kosync_push_local_once(&push_config, &output.push);
    output.stage = CN_SYNC_PUSH_EXECUTED;
    output.outcome = classify_push(&output.push);
    output.remote_mutation = output.push.remote_mutation;
done:
    cn_credentials_clear(&credentials);
    clear_bytes(&client, sizeof client);
    return output;
}

const char *cn_sync_push_stage_name(cn_sync_push_stage stage)
{
    static const char *const names[] = {
        "invalid", "disabled", "settings-failed", "credentials-failed",
        "config-failed", "executed"
    };
    return stage >= CN_SYNC_PUSH_INVALID && stage <= CN_SYNC_PUSH_EXECUTED
               ? names[stage] : "unknown";
}

const char *cn_sync_push_outcome_name(cn_sync_push_outcome outcome)
{
    static const char *const names[CN_SYNC_PUSH_OUTCOME_COUNT] = {
        "internal-failure", "disabled", "uploaded", "local-missing",
        "local-unsupported", "local-failure", "auth-required",
        "trusted-time-unavailable", "connectivity-failure",
        "security-failure", "service-failure", "configuration-failure"
    };
    return outcome >= 0 && outcome < CN_SYNC_PUSH_OUTCOME_COUNT
               ? names[outcome] : "unknown";
}
