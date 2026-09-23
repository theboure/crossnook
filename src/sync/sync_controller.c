#include <string.h>

#include "sync/sync_controller.h"

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (length--) *p++ = 0;
}

static cn_sync_controller_result initial_result(void)
{
    cn_sync_controller_result result;
    memset(&result, 0, sizeof result);
    result.stage = CN_SYNC_CONTROLLER_INVALID;
    result.product.outcome = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
    result.product.retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
    result.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    result.credential_result = CN_CREDENTIAL_INVALID;
    result.sync_status = CN_KOSYNC_SYNC_STATUS_INVALID;
    result.decision = CN_KOSYNC_SYNC_DECISION_NONE;
    result.local_load_result = CN_PROGRESS_INVALID;
    result.local_save_result = CN_PROGRESS_INVALID;
    result.time_result = CN_TIME_INVALID;
    result.dns_result = CN_DNS_INVALID;
    result.kosync_result = CN_KOSYNC_INVALID;
    result.transport_result = CN_NETSIMPLE_INVALID;
    return result;
}

/* Match the KOSync device_id header and UTF-8 rules before network activity. */
static int valid_device_id(const char *text)
{
    size_t i = 0;
    if (!text) return 0;
    while (text[i]) {
        unsigned char ch = (unsigned char)text[i++];
        unsigned value, minimum;
        int remaining;
        if (i > CN_KOSYNC_DEVICE_MAX || ch < 0x20 || ch == 0x7f) return 0;
        if (ch < 0x80) continue;
        if (ch >= 0xc2 && ch <= 0xdf) {
            value = ch & 0x1f; minimum = 0x80; remaining = 1;
        } else if (ch >= 0xe0 && ch <= 0xef) {
            value = ch & 0x0f; minimum = 0x800; remaining = 2;
        } else if (ch >= 0xf0 && ch <= 0xf4) {
            value = ch & 0x07; minimum = 0x10000; remaining = 3;
        } else return 0;
        while (remaining-- > 0) {
            unsigned char next;
            if (i >= CN_KOSYNC_DEVICE_MAX || !text[i]) return 0;
            next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80) return 0;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return i != 0;
}

static void preflight_failure(cn_sync_controller_result *result,
                              cn_sync_controller_stage stage,
                              cn_kosync_product_outcome outcome)
{
    result->stage = stage;
    result->product.outcome = outcome;
    result->product.retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
}

cn_sync_controller_result cn_sync_current_book(
    const cn_sync_controller_config *config)
{
    cn_sync_controller_result output = initial_result();
    cn_settings settings;
    cn_credentials credentials;
    cn_kosync_client client;
    cn_kosync_sync_config sync_config;
    cn_kosync_sync_result sync;
    cn_kosync_result client_result;
    int classified;

    if (!config || !config->settings_store) return output;
    output.settings_result = cn_settings_load(config->settings_store,
                                               &settings, NULL);
    if (output.settings_result != CN_SETTINGS_OK &&
        output.settings_result != CN_SETTINGS_MISSING) {
        preflight_failure(&output, CN_SYNC_CONTROLLER_SETTINGS_FAILED,
                          CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE);
        return output;
    }
    if (output.settings_result == CN_SETTINGS_MISSING ||
        !settings.kosync_enabled) {
        output.stage = CN_SYNC_CONTROLLER_DISABLED;
        if (cn_kosync_policy_classify(0, NULL, &output.product) != 0)
            output.stage = CN_SYNC_CONTROLLER_CLASSIFICATION_FAILED;
        return output;
    }
    if (!config->credential_store) {
        preflight_failure(&output, CN_SYNC_CONTROLLER_CONFIG_FAILED,
                          CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE);
        return output;
    }
    memset(&credentials, 0, sizeof credentials);
    memset(&client, 0, sizeof client);
    output.credential_result = cn_credential_store_load(
        config->credential_store, &credentials, NULL);
    if (output.credential_result != CN_CREDENTIAL_OK) {
        preflight_failure(&output, CN_SYNC_CONTROLLER_CREDENTIALS_FAILED,
                          output.credential_result == CN_CREDENTIAL_MISSING
                              ? CN_KOSYNC_PRODUCT_AUTH_REQUIRED
                              : output.credential_result == CN_CREDENTIAL_CORRUPT ||
                                output.credential_result == CN_CREDENTIAL_UNSUPPORTED_VERSION ||
                                output.credential_result == CN_CREDENTIAL_INVALID
                                    ? CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE
                                    : CN_KOSYNC_PRODUCT_LOCAL_FAILURE);
        goto done;
    }
    if (cn_settings_validate(&settings) != CN_SETTINGS_OK ||
        !config->progress_store || !config->document_path ||
        !config->document_path[0] || !valid_device_id(config->device_id) ||
        !config->dns || !config->tls || !config->tls->ca_path ||
        (config->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         config->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED) ||
        (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         (!config->time || config->tls->get_time))) {
        preflight_failure(&output, CN_SYNC_CONTROLLER_CONFIG_FAILED,
                          CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE);
        goto done;
    }
    client_result = cn_kosync_client_init(
        &client, settings.kosync_base_url, credentials.username,
        credentials.userkey);
    if (client_result != CN_KOSYNC_OK || !client.use_tls) {
        preflight_failure(&output, CN_SYNC_CONTROLLER_CONFIG_FAILED,
                          CN_KOSYNC_PRODUCT_CONFIGURATION_FAILURE);
        goto done;
    }
    /* Integration owns identity, local load, time, DNS, GET, and any write. */
    memset(&sync_config, 0, sizeof sync_config);
    sync_config.document_path = config->document_path;
    sync_config.progress_store = config->progress_store;
    sync_config.time_policy = config->time_policy;
    sync_config.time = config->time;
    sync_config.dns = config->dns;
    sync_config.tls = config->tls;
    sync_config.client = &client;
    sync_config.device = settings.kosync_device_name;
    sync_config.device_id = config->device_id;
    cn_kosync_sync_result_init(&sync);
    (void)cn_kosync_sync_once(&sync_config, &sync);
    output.stage = CN_SYNC_CONTROLLER_INTEGRATION;
    output.sync_status = sync.status;
    output.decision = sync.decision;
    output.local_load_result = sync.local_load_result;
    output.local_save_result = sync.local_save_result;
    output.time_result = sync.time_result;
    output.dns_result = sync.dns_result;
    output.kosync_result = sync.kosync_result;
    output.transport_result = sync.kosync_outcome.transport_result;
    output.local_save_attempted = sync.local_save_attempted;
    output.remote_put_attempted = sync.remote_put_attempted;
    output.local_saved = sync.local_saved;
    output.remote_uploaded = sync.remote_uploaded;
    classified = cn_kosync_policy_classify(1, &sync, &output.product);
    if (classified != 0) {
        output.stage = CN_SYNC_CONTROLLER_CLASSIFICATION_FAILED;
        output.product.outcome = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
        output.product.retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
        output.product.local_mutation = sync.local_saved && sync.local_save_attempted
                                            ? CN_KOSYNC_MUTATION_CONFIRMED
                                            : sync.local_saved || sync.local_save_attempted
                                                  ? CN_KOSYNC_MUTATION_POSSIBLE
                                                  : CN_KOSYNC_MUTATION_NONE;
        output.product.remote_mutation = sync.remote_uploaded && sync.remote_put_attempted
                                             ? CN_KOSYNC_MUTATION_CONFIRMED
                                             : sync.remote_uploaded || sync.remote_put_attempted
                                                   ? CN_KOSYNC_MUTATION_POSSIBLE
                                                   : CN_KOSYNC_MUTATION_NONE;
        output.mutation_evidence_conservative = 1;
    }
    cn_kosync_sync_result_clear(&sync);
done:
    cn_credentials_clear(&credentials);
    clear_bytes(&client, sizeof client);
    return output;
}

const char *cn_sync_controller_stage_name(cn_sync_controller_stage stage)
{
    static const char *const names[] = {
        "invalid", "disabled", "settings-failed", "credentials-failed",
        "config-failed", "integration", "classification-failed"
    };
    return stage >= CN_SYNC_CONTROLLER_INVALID &&
           stage <= CN_SYNC_CONTROLLER_CLASSIFICATION_FAILED
               ? names[stage] : "unknown";
}
