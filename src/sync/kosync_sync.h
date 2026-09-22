/* Synchronous composition of local progress and verified KOSync transport. */
#ifndef CN_SYNC_KOSYNC_SYNC_H
#define CN_SYNC_KOSYNC_SYNC_H

#include "book/koreader_identity.h"
#include "net/dnssimple.h"
#include "net/tlssimple.h"
#include "progress/progress_store.h"
#include "sync/kosync.h"
#include "time/timesimple.h"

typedef enum cn_kosync_sync_time_policy {
    /* Synchronize CLOCK_REALTIME; tls->get_time must be NULL. */
    CN_KOSYNC_SYNC_TIME_ESTABLISH = 0,
    /* The caller guarantees that the TLS time source is already valid. */
    CN_KOSYNC_SYNC_TIME_CALLER_ESTABLISHED
} cn_kosync_sync_time_policy;

typedef enum cn_kosync_sync_status {
    CN_KOSYNC_SYNC_STATUS_OK = 0,
    CN_KOSYNC_SYNC_STATUS_INVALID,
    CN_KOSYNC_SYNC_STATUS_IDENTITY_FAILED,
    CN_KOSYNC_SYNC_STATUS_LOCAL_FAILED,
    CN_KOSYNC_SYNC_STATUS_LOCAL_UNSUPPORTED,
    CN_KOSYNC_SYNC_STATUS_TRUSTED_TIME_FAILED,
    CN_KOSYNC_SYNC_STATUS_DNS_FAILED,
    CN_KOSYNC_SYNC_STATUS_AUTH_FAILED,
    CN_KOSYNC_SYNC_STATUS_HTTPS_FAILED,
    CN_KOSYNC_SYNC_STATUS_PROTOCOL_FAILED,
    CN_KOSYNC_SYNC_STATUS_NO_MEMORY,
    CN_KOSYNC_SYNC_STATUS_COUNT
} cn_kosync_sync_status;

typedef enum cn_kosync_sync_decision {
    CN_KOSYNC_SYNC_DECISION_NONE = 0,
    CN_KOSYNC_SYNC_NO_STATE,
    CN_KOSYNC_SYNC_LOCAL_SELECTED,
    CN_KOSYNC_SYNC_REMOTE_SELECTED,
    CN_KOSYNC_SYNC_NO_CHANGE,
    CN_KOSYNC_SYNC_AMBIGUOUS,
    CN_KOSYNC_SYNC_DECISION_COUNT
} cn_kosync_sync_decision;

typedef struct cn_kosync_sync_config {
    const char *document_path;
    cn_progress_store *progress_store;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    const cn_kosync_client *client;
    const char *device;
    const char *device_id;
} cn_kosync_sync_config;

typedef struct cn_kosync_sync_result {
    cn_kosync_sync_status status;
    cn_kosync_sync_decision decision;
    int local_present;
    int remote_present;
    int local_saved;
    int remote_uploaded;

    cn_koreader_document_id document_id;
    /* Owns logical_position. Initialize and clear the containing result. */
    cn_kosync_progress remote_progress;

    int local_identity_ok;
    cn_koreader_identity_result identity_result;
    cn_progress_result local_load_result;
    cn_progress_result local_save_result;
    cn_time_result time_result;
    cn_dns_result dns_result;
    cn_kosync_result kosync_result;
    cn_kosync_outcome kosync_outcome;

    long long put_timestamp;
    int has_put_timestamp;
} cn_kosync_sync_result;

void cn_kosync_sync_result_init(cn_kosync_sync_result *result);
void cn_kosync_sync_result_clear(cn_kosync_sync_result *result);

/* result must be initialized before this call and cleared when finished. */
cn_kosync_sync_status cn_kosync_sync_once(
    const cn_kosync_sync_config *config,
    cn_kosync_sync_result *result);

const char *cn_kosync_sync_status_name(cn_kosync_sync_status status);
const char *cn_kosync_sync_decision_name(cn_kosync_sync_decision decision);

#endif /* CN_SYNC_KOSYNC_SYNC_H */
