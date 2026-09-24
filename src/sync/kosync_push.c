#include <string.h>

#include "progress/book_identity.h"
#include "sync/kosync_push.h"

static void clear_client_copy(cn_kosync_client *client)
{
    volatile unsigned char *p = (volatile unsigned char *)client;
    size_t length = sizeof *client;
    while (length--) *p++ = 0;
}

/* These statuses are emitted by connect or TLS open, before HTTP request
 * bytes can be sent. All other failures after invoking PUT stay POSSIBLE. */
static int proven_pre_send(cn_netsimple_result transport)
{
    switch (transport) {
    case CN_NETSIMPLE_RESOLVE_ERROR:
    case CN_NETSIMPLE_CONNECT_REFUSED:
    case CN_NETSIMPLE_NETWORK_UNREACHABLE:
    case CN_NETSIMPLE_HOST_UNREACHABLE:
    case CN_NETSIMPLE_CONNECT_TIMEOUT:
    case CN_NETSIMPLE_CONNECT_ERROR:
    case CN_NETSIMPLE_TLS_ENTROPY_FAILED:
    case CN_NETSIMPLE_TLS_INVALID_CA:
    case CN_NETSIMPLE_TLS_HANDSHAKE_FAILED:
    case CN_NETSIMPLE_TLS_HANDSHAKE_TIMEOUT:
    case CN_NETSIMPLE_TLS_TRUST_FAILED:
    case CN_NETSIMPLE_TLS_HOSTNAME_MISMATCH:
    case CN_NETSIMPLE_TLS_CERT_TIME_FAILED:
    case CN_NETSIMPLE_TLS_CERT_INVALID:
        return 1;
    default:
        return 0;
    }
}

void cn_kosync_push_result_init(cn_kosync_push_result *result)
{
    if (!result) return;
    memset(result, 0, sizeof *result);
    result->status = CN_KOSYNC_PUSH_INVALID;
    result->identity_result = CN_KOREADER_IDENTITY_INVALID;
    result->local_load_result = CN_PROGRESS_INVALID;
    result->time_result = CN_TIME_INVALID;
    result->dns_result = CN_DNS_INVALID;
    result->kosync_result = CN_KOSYNC_INVALID;
    result->kosync_outcome.transport_result = CN_NETSIMPLE_INVALID;
}

cn_kosync_push_status cn_kosync_push_local_once(
    const cn_kosync_push_config *config, cn_kosync_push_result *result)
{
    cn_book_identity local_identity;
    cn_koreader_document_id remote_identity;
    cn_progress_record local;
    cn_kosync_progress upload;
    cn_dns_answer answer;
    cn_time_sample sample;
    cn_kosync_client client;
    const char *document_id;
    cn_kosync_push_status status = CN_KOSYNC_PUSH_INVALID;
    int client_copied = 0;

    if (!result) return CN_KOSYNC_PUSH_INVALID;
    cn_kosync_push_result_init(result);
    if (!config || !config->document_path || !config->progress_store ||
        !config->dns || !config->tls || !config->tls->ca_path ||
        !config->client || !config->client->use_tls ||
        !config->device || !config->device_id ||
        (config->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         config->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED) ||
        (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         (!config->time || config->tls->get_time)))
        return CN_KOSYNC_PUSH_INVALID;

    cn_book_identity_init(&local_identity);
    if (cn_book_identity_from_path(&local_identity, config->document_path) != 0) {
        status = CN_KOSYNC_PUSH_IDENTITY_FAILED;
        goto done;
    }
    result->identity_result = cn_book_identity_koreader_binary(
        config->document_path, &remote_identity);
    if (result->identity_result != CN_KOREADER_IDENTITY_OK) {
        status = CN_KOSYNC_PUSH_IDENTITY_FAILED;
        goto done;
    }
    document_id = cn_koreader_document_id_text(&remote_identity);
    if (!document_id) {
        status = CN_KOSYNC_PUSH_IDENTITY_FAILED;
        goto done;
    }

    cn_progress_record_init(&local);
    cn_kosync_progress_init(&upload);
    result->local_load_result = cn_progress_store_load(
        config->progress_store, &local_identity, &local);
    if (result->local_load_result != CN_PROGRESS_OK) {
        status = result->local_load_result == CN_PROGRESS_MISSING
                     ? CN_KOSYNC_PUSH_LOCAL_MISSING
                     : result->local_load_result == CN_PROGRESS_NO_MEMORY
                           ? CN_KOSYNC_PUSH_NO_MEMORY
                           : CN_KOSYNC_PUSH_LOCAL_FAILED;
        goto clear_local;
    }
    if (local.position.progress_10000 < 0) {
        status = CN_KOSYNC_PUSH_LOCAL_UNSUPPORTED;
        goto clear_local;
    }
    result->kosync_result = cn_kosync_progress_set(
        &upload, document_id, local.position.location,
        local.position.progress_10000, config->device, config->device_id);
    if (result->kosync_result != CN_KOSYNC_OK) {
        status = result->kosync_result == CN_KOSYNC_NO_MEMORY
                     ? CN_KOSYNC_PUSH_NO_MEMORY : CN_KOSYNC_PUSH_INVALID;
        goto clear_local;
    }
    /* No network operation is permitted until the persisted record and
     * runtime device fields have passed the existing KOSync conversion. */
    if (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH) {
        result->time_result = cn_timesimple_sync(config->time, &sample);
        if (result->time_result != CN_TIME_OK) {
            status = CN_KOSYNC_PUSH_TRUSTED_TIME_FAILED;
            goto clear_local;
        }
    } else result->time_result = CN_TIME_OK;

    result->dns_result = cn_dnssimple_resolve_a(
        config->dns, config->client->host, &answer);
    if (result->dns_result != CN_DNS_OK) {
        status = CN_KOSYNC_PUSH_DNS_FAILED;
        goto clear_local;
    }
    if (answer.count == 0) {
        result->dns_result = CN_DNS_NO_ADDRESS;
        status = CN_KOSYNC_PUSH_DNS_FAILED;
        goto clear_local;
    }
    client = *config->client;
    client_copied = 1;
    result->kosync_result = cn_kosync_client_set_tls(
        &client, config->tls, answer.ipv4[0]);
    if (result->kosync_result != CN_KOSYNC_OK) {
        status = CN_KOSYNC_PUSH_INVALID;
        goto clear_local;
    }

    result->put_invoked = 1;
    result->kosync_result = cn_kosync_put_progress(
        &client, &upload, NULL, &result->kosync_outcome);
    if (result->kosync_result == CN_KOSYNC_OK) {
        result->remote_uploaded = 1;
        result->remote_mutation = CN_KOSYNC_MUTATION_CONFIRMED;
        status = CN_KOSYNC_PUSH_UPLOADED;
    } else {
        result->remote_mutation = CN_KOSYNC_MUTATION_POSSIBLE;
        if (result->kosync_result == CN_KOSYNC_TRANSPORT_ERROR &&
            proven_pre_send(result->kosync_outcome.transport_result))
            result->remote_mutation = CN_KOSYNC_MUTATION_NONE;
        switch (result->kosync_result) {
        case CN_KOSYNC_AUTH_FAILED: status = CN_KOSYNC_PUSH_AUTH_FAILED; break;
        case CN_KOSYNC_TRANSPORT_ERROR: status = CN_KOSYNC_PUSH_HTTPS_FAILED; break;
        case CN_KOSYNC_NO_MEMORY: status = CN_KOSYNC_PUSH_NO_MEMORY; break;
        case CN_KOSYNC_INVALID: status = CN_KOSYNC_PUSH_INVALID; break;
        default: status = CN_KOSYNC_PUSH_PROTOCOL_FAILED; break;
        }
    }
clear_local:
    cn_kosync_progress_clear(&upload);
    cn_progress_record_clear(&local);
done:
    if (client_copied) clear_client_copy(&client);
    result->status = status;
    return status;
}

const char *cn_kosync_push_status_name(cn_kosync_push_status status)
{
    static const char *const names[CN_KOSYNC_PUSH_STATUS_COUNT] = {
        "invalid", "uploaded", "identity-failed", "local-missing",
        "local-failed", "local-unsupported", "trusted-time-failed",
        "dns-failed", "auth-failed", "https-failed", "protocol-failed",
        "no-memory"
    };
    return status >= 0 && status < CN_KOSYNC_PUSH_STATUS_COUNT
               ? names[status] : "unknown";
}
