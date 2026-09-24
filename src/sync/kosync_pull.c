#include <string.h>

#include "progress/book_identity.h"
#include "sync/kosync_pull.h"

static void clear_client_copy(cn_kosync_client *client)
{
    volatile unsigned char *p = (volatile unsigned char *)client;
    size_t length = sizeof *client;
    while (length--) *p++ = 0;
}

void cn_kosync_pull_result_init(cn_kosync_pull_result *result)
{
    if (!result) return;
    memset(result, 0, sizeof *result);
    result->status = CN_KOSYNC_PULL_INVALID;
    result->identity_result = CN_KOREADER_IDENTITY_INVALID;
    result->time_result = CN_TIME_INVALID;
    result->dns_result = CN_DNS_INVALID;
    result->kosync_result = CN_KOSYNC_INVALID;
    result->kosync_outcome.transport_result = CN_NETSIMPLE_INVALID;
    result->local_save_result = CN_PROGRESS_INVALID;
}

cn_kosync_pull_status cn_kosync_pull_remote_once(
    const cn_kosync_pull_config *config, cn_kosync_pull_result *result)
{
    cn_book_identity local_identity;
    cn_koreader_document_id remote_identity;
    const char *document_id;
    cn_time_sample sample;
    cn_dns_answer answer;
    cn_kosync_client client;
    cn_kosync_progress remote;
    cn_progress_record record;
    cn_kosync_pull_status status = CN_KOSYNC_PULL_INVALID;
    int client_copied = 0;

    if (!result) return CN_KOSYNC_PULL_INVALID;
    cn_kosync_pull_result_init(result);
    if (!config || !config->document_path || !config->progress_store ||
        !config->dns || !config->tls || !config->tls->ca_path ||
        !config->client || !config->client->use_tls ||
        (config->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         config->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED) ||
        (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         (!config->time || config->tls->get_time)))
        return CN_KOSYNC_PULL_INVALID;

    cn_book_identity_init(&local_identity);
    if (cn_book_identity_from_path(&local_identity, config->document_path) != 0) {
        status = CN_KOSYNC_PULL_IDENTITY_FAILED;
        goto done;
    }
    result->identity_result = cn_book_identity_koreader_binary(
        config->document_path, &remote_identity);
    if (result->identity_result != CN_KOREADER_IDENTITY_OK) {
        status = CN_KOSYNC_PULL_IDENTITY_FAILED;
        goto done;
    }
    document_id = cn_koreader_document_id_text(&remote_identity);
    if (!document_id) {
        status = CN_KOSYNC_PULL_IDENTITY_FAILED;
        goto done;
    }
    if (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH) {
        result->time_result = cn_timesimple_sync(config->time, &sample);
        if (result->time_result != CN_TIME_OK) {
            status = CN_KOSYNC_PULL_TRUSTED_TIME_FAILED;
            goto done;
        }
    } else result->time_result = CN_TIME_OK;

    result->dns_result = cn_dnssimple_resolve_a(
        config->dns, config->client->host, &answer);
    if (result->dns_result != CN_DNS_OK) {
        status = CN_KOSYNC_PULL_DNS_FAILED;
        goto done;
    }
    if (answer.count == 0) {
        result->dns_result = CN_DNS_NO_ADDRESS;
        status = CN_KOSYNC_PULL_DNS_FAILED;
        goto done;
    }
    client = *config->client;
    client_copied = 1;
    result->kosync_result = cn_kosync_client_set_tls(
        &client, config->tls, answer.ipv4[0]);
    if (result->kosync_result != CN_KOSYNC_OK) {
        status = CN_KOSYNC_PULL_INVALID;
        goto done;
    }
    cn_kosync_progress_init(&remote);
    result->get_attempted = 1;
    result->kosync_result = cn_kosync_get_progress(
        &client, document_id, &remote, &result->kosync_outcome);
    if (result->kosync_result == CN_KOSYNC_NOT_FOUND) {
        status = CN_KOSYNC_PULL_REMOTE_MISSING;
    } else if (result->kosync_result == CN_KOSYNC_OK) {
        /* Borrow the GET-owned position only across this synchronous save.
         * Do not persist remote device/device_id/timestamp. */
        cn_progress_record_init(&record);
        record.position.location = remote.logical_position;
        record.position.progress_10000 = remote.progress_10000;
        result->local_save_attempted = 1;
        result->local_save_result = cn_progress_store_save(
            config->progress_store, &local_identity, &record);
        record.position.location = NULL;
        cn_progress_record_clear(&record);
        if (result->local_save_result == CN_PROGRESS_OK) {
            result->local_saved = 1;
            status = CN_KOSYNC_PULL_PERSISTED;
        } else status = CN_KOSYNC_PULL_LOCAL_FAILED;
    } else if (result->kosync_result == CN_KOSYNC_AUTH_FAILED) {
        status = CN_KOSYNC_PULL_AUTH_FAILED;
    } else if (result->kosync_result == CN_KOSYNC_TRANSPORT_ERROR) {
        status = CN_KOSYNC_PULL_HTTPS_FAILED;
    } else if (result->kosync_result == CN_KOSYNC_NO_MEMORY) {
        status = CN_KOSYNC_PULL_NO_MEMORY;
    } else if (result->kosync_result == CN_KOSYNC_INVALID) {
        status = CN_KOSYNC_PULL_INVALID;
    } else status = CN_KOSYNC_PULL_PROTOCOL_FAILED;
    cn_kosync_progress_clear(&remote);
done:
    if (client_copied) clear_client_copy(&client);
    result->status = status;
    return status;
}

const char *cn_kosync_pull_status_name(cn_kosync_pull_status status)
{
    static const char *const names[CN_KOSYNC_PULL_STATUS_COUNT] = {
        "invalid", "remote-missing", "persisted", "identity-failed",
        "trusted-time-failed", "dns-failed", "auth-failed", "https-failed",
        "protocol-failed", "local-failed", "no-memory"
    };
    return status >= 0 && status < CN_KOSYNC_PULL_STATUS_COUNT
               ? names[status] : "unknown";
}
