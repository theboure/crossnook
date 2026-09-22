#include <string.h>

#include "progress/book_identity.h"
#include "sync/kosync_sync.h"

static cn_kosync_sync_status finish(cn_kosync_sync_result *result,
                                    cn_kosync_sync_status status)
{
    result->status = status;
    return status;
}

static void clear_client_copy(cn_kosync_client *client)
{
    volatile unsigned char *p = (volatile unsigned char *)client;
    size_t length = sizeof *client;
    while (length--)
        *p++ = 0;
}

static cn_kosync_sync_status map_kosync(cn_kosync_result result)
{
    switch (result) {
    case CN_KOSYNC_AUTH_FAILED:
        return CN_KOSYNC_SYNC_STATUS_AUTH_FAILED;
    case CN_KOSYNC_TRANSPORT_ERROR:
        return CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED;
    case CN_KOSYNC_HTTP_ERROR:
    case CN_KOSYNC_BAD_JSON:
    case CN_KOSYNC_BAD_PROTOCOL:
        return CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED;
    case CN_KOSYNC_NO_MEMORY:
        return CN_KOSYNC_SYNC_STATUS_NO_MEMORY;
    default:
        return CN_KOSYNC_SYNC_STATUS_INVALID;
    }
}

void cn_kosync_sync_result_init(cn_kosync_sync_result *result)
{
    if (!result)
        return;
    memset(result, 0, sizeof *result);
    result->status = CN_KOSYNC_SYNC_STATUS_INVALID;
    result->decision = CN_KOSYNC_SYNC_DECISION_NONE;
    result->identity_result = CN_KOREADER_IDENTITY_INVALID;
    result->local_load_result = CN_PROGRESS_INVALID;
    result->local_save_result = CN_PROGRESS_INVALID;
    result->time_result = CN_TIME_INVALID;
    result->dns_result = CN_DNS_INVALID;
    result->kosync_result = CN_KOSYNC_INVALID;
    result->kosync_outcome.transport_result = CN_NETSIMPLE_INVALID;
    cn_koreader_document_id_init(&result->document_id);
    cn_kosync_progress_init(&result->remote_progress);
}

void cn_kosync_sync_result_clear(cn_kosync_sync_result *result)
{
    if (!result)
        return;
    cn_kosync_progress_clear(&result->remote_progress);
    cn_kosync_sync_result_init(result);
}

cn_kosync_sync_status cn_kosync_sync_once(
    const cn_kosync_sync_config *config,
    cn_kosync_sync_result *result)
{
    cn_book_identity local_identity;
    cn_progress_record local;
    cn_kosync_client client;
    cn_kosync_progress upload;
    cn_dns_answer answer;
    cn_time_sample sample;
    cn_progress_record remote_record;
    cn_progress_result progress_result;
    const char *document_id;
    int local_initialized = 0;
    int upload_initialized = 0;
    int client_copied = 0;
    cn_kosync_sync_status status;

    if (!result)
        return CN_KOSYNC_SYNC_STATUS_INVALID;
    cn_kosync_sync_result_clear(result);
    if (!config || !config->document_path || !config->progress_store ||
        !config->dns || !config->tls || !config->client ||
        !config->device || !config->device_id ||
        !config->client->use_tls ||
        (config->time_policy != CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         config->time_policy != CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED) ||
        (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH &&
         (!config->time || config->tls->get_time)))
        return finish(result, CN_KOSYNC_SYNC_STATUS_INVALID);

    cn_book_identity_init(&local_identity);
    result->local_identity_ok =
        cn_book_identity_from_path(&local_identity, config->document_path) == 0;
    if (!result->local_identity_ok)
        return finish(result, CN_KOSYNC_SYNC_STATUS_IDENTITY_FAILED);

    result->identity_result = cn_book_identity_koreader_binary(
        config->document_path, &result->document_id);
    if (result->identity_result != CN_KOREADER_IDENTITY_OK)
        return finish(result, CN_KOSYNC_SYNC_STATUS_IDENTITY_FAILED);
    document_id = cn_koreader_document_id_text(&result->document_id);
    if (!document_id)
        return finish(result, CN_KOSYNC_SYNC_STATUS_IDENTITY_FAILED);

    cn_progress_record_init(&local);
    local_initialized = 1;
    result->local_load_result = cn_progress_store_load(
        config->progress_store, &local_identity, &local);
    if (result->local_load_result == CN_PROGRESS_OK) {
        result->local_present = 1;
    } else if (result->local_load_result != CN_PROGRESS_MISSING) {
        status = result->local_load_result == CN_PROGRESS_NO_MEMORY
                     ? CN_KOSYNC_SYNC_STATUS_NO_MEMORY
                     : CN_KOSYNC_SYNC_STATUS_LOCAL_FAILED;
        goto done;
    }

    if (config->time_policy == CN_KOSYNC_SYNC_TIME_ESTABLISH) {
        result->time_result = cn_timesimple_sync(config->time, &sample);
        if (result->time_result != CN_TIME_OK) {
            status = CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED;
            goto done;
        }
    } else {
        result->time_result = CN_TIME_OK;
    }

    result->dns_result = cn_dnssimple_resolve_a(
        config->dns, config->client->host, &answer);
    if (result->dns_result != CN_DNS_OK) {
        status = CN_KOSYNC_SYNC_STATUS_DNS_FAILED;
        goto done;
    }
    if (answer.count == 0) {
        result->dns_result = CN_DNS_NO_ADDRESS;
        status = CN_KOSYNC_SYNC_STATUS_DNS_FAILED;
        goto done;
    }

    client = *config->client;
    client_copied = 1;
    result->kosync_result = cn_kosync_client_set_tls(
        &client, config->tls, answer.ipv4[0]);
    if (result->kosync_result != CN_KOSYNC_OK) {
        status = map_kosync(result->kosync_result);
        goto done;
    }
    result->kosync_result = cn_kosync_get_progress(
        &client, document_id, &result->remote_progress,
        &result->kosync_outcome);
    if (result->kosync_result == CN_KOSYNC_OK) {
        result->remote_present = 1;
    } else if (result->kosync_result != CN_KOSYNC_NOT_FOUND) {
        status = map_kosync(result->kosync_result);
        goto done;
    }

    if (!result->local_present && !result->remote_present) {
        result->decision = CN_KOSYNC_SYNC_NO_STATE;
        status = CN_KOSYNC_SYNC_STATUS_OK;
        goto done;
    }
    if (!result->local_present) {
        result->decision = CN_KOSYNC_SYNC_REMOTE_SELECTED;
        cn_progress_record_init(&remote_record);
        remote_record.position.location =
            result->remote_progress.logical_position;
        remote_record.position.progress_10000 =
            result->remote_progress.progress_10000;
        result->local_save_attempted = 1;
        progress_result = cn_progress_store_save(
            config->progress_store, &local_identity, &remote_record);
        remote_record.position.location = NULL;
        result->local_save_result = progress_result;
        if (progress_result != CN_PROGRESS_OK) {
            status = progress_result == CN_PROGRESS_NO_MEMORY
                         ? CN_KOSYNC_SYNC_STATUS_NO_MEMORY
                         : CN_KOSYNC_SYNC_STATUS_LOCAL_FAILED;
            goto done;
        }
        result->local_saved = 1;
        status = CN_KOSYNC_SYNC_STATUS_OK;
        goto done;
    }
    if (!result->remote_present) {
        result->decision = CN_KOSYNC_SYNC_LOCAL_SELECTED;
        if (local.position.progress_10000 < 0) {
            status = CN_KOSYNC_SYNC_STATUS_LOCAL_UNSUPPORTED;
            goto done;
        }
        cn_kosync_progress_init(&upload);
        upload_initialized = 1;
        result->kosync_result = cn_kosync_progress_set(
            &upload, document_id, local.position.location,
            local.position.progress_10000, config->device, config->device_id);
        if (result->kosync_result != CN_KOSYNC_OK) {
            status = map_kosync(result->kosync_result);
            goto done;
        }
        result->remote_put_attempted = 1;
        result->kosync_result = cn_kosync_put_progress(
            &client, &upload, &result->put_timestamp,
            &result->kosync_outcome);
        if (result->kosync_result != CN_KOSYNC_OK) {
            status = map_kosync(result->kosync_result);
            goto done;
        }
        result->remote_uploaded = 1;
        result->has_put_timestamp = 1;
        status = CN_KOSYNC_SYNC_STATUS_OK;
        goto done;
    }

    if (strcmp(local.position.location,
               result->remote_progress.logical_position) == 0)
        result->decision = CN_KOSYNC_SYNC_NO_CHANGE;
    else
        result->decision = CN_KOSYNC_SYNC_AMBIGUOUS;
    status = CN_KOSYNC_SYNC_STATUS_OK;

done:
    if (upload_initialized)
        cn_kosync_progress_clear(&upload);
    if (local_initialized)
        cn_progress_record_clear(&local);
    if (client_copied)
        clear_client_copy(&client);
    return finish(result, status);
}

const char *cn_kosync_sync_status_name(cn_kosync_sync_status status)
{
    static const char *const names[CN_KOSYNC_SYNC_STATUS_COUNT] = {
        "ok", "invalid", "identity-failed", "local-failed",
        "local-unsupported", "trusted-time-failed", "dns-failed",
        "auth-failed", "https-failed", "protocol-failed", "no-memory"
    };
    if (status < 0 || status >= CN_KOSYNC_SYNC_STATUS_COUNT)
        return "unknown";
    return names[status];
}

const char *cn_kosync_sync_decision_name(cn_kosync_sync_decision decision)
{
    static const char *const names[CN_KOSYNC_SYNC_DECISION_COUNT] = {
        "none", "no-state", "local-selected", "remote-selected",
        "no-change", "ambiguous"
    };
    if (decision < 0 || decision >= CN_KOSYNC_SYNC_DECISION_COUNT)
        return "unknown";
    return names[decision];
}
