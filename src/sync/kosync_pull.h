/* Explicit GET-only remote progress overwrite; separate from normal sync. */
#ifndef CN_SYNC_KOSYNC_PULL_H
#define CN_SYNC_KOSYNC_PULL_H

#include "sync/kosync_sync.h"

typedef enum cn_kosync_pull_status {
    CN_KOSYNC_PULL_INVALID = 0,
    CN_KOSYNC_PULL_REMOTE_MISSING,
    CN_KOSYNC_PULL_PERSISTED,
    CN_KOSYNC_PULL_IDENTITY_FAILED,
    CN_KOSYNC_PULL_TRUSTED_TIME_FAILED,
    CN_KOSYNC_PULL_DNS_FAILED,
    CN_KOSYNC_PULL_AUTH_FAILED,
    CN_KOSYNC_PULL_HTTPS_FAILED,
    CN_KOSYNC_PULL_PROTOCOL_FAILED,
    CN_KOSYNC_PULL_LOCAL_FAILED,
    CN_KOSYNC_PULL_NO_MEMORY,
    CN_KOSYNC_PULL_STATUS_COUNT
} cn_kosync_pull_status;

typedef struct cn_kosync_pull_config {
    const char *document_path;
    cn_progress_store *progress_store;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    const cn_kosync_client *client; /* HTTPS-only; owned by caller */
} cn_kosync_pull_config;

/* Bounded diagnostic evidence only: no document path, credential or XPointer. */
typedef struct cn_kosync_pull_result {
    cn_kosync_pull_status status;
    cn_koreader_identity_result identity_result;
    cn_time_result time_result;
    cn_dns_result dns_result;
    cn_kosync_result kosync_result;
    cn_kosync_outcome kosync_outcome;
    cn_progress_result local_save_result;
    int get_attempted;
    int local_save_attempted;
    int local_saved;
} cn_kosync_pull_result;

void cn_kosync_pull_result_init(cn_kosync_pull_result *result);
cn_kosync_pull_status cn_kosync_pull_remote_once(
    const cn_kosync_pull_config *config, cn_kosync_pull_result *result);
const char *cn_kosync_pull_status_name(cn_kosync_pull_status status);

#endif /* CN_SYNC_KOSYNC_PULL_H */
