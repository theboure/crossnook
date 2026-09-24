/* Explicit PUT-only upload of an already-persisted local position. */
#ifndef CN_SYNC_KOSYNC_PUSH_H
#define CN_SYNC_KOSYNC_PUSH_H

#include "sync/kosync_policy.h"

typedef enum cn_kosync_push_status {
    CN_KOSYNC_PUSH_INVALID = 0,
    CN_KOSYNC_PUSH_UPLOADED,
    CN_KOSYNC_PUSH_IDENTITY_FAILED,
    CN_KOSYNC_PUSH_LOCAL_MISSING,
    CN_KOSYNC_PUSH_LOCAL_FAILED,
    CN_KOSYNC_PUSH_LOCAL_UNSUPPORTED,
    CN_KOSYNC_PUSH_TRUSTED_TIME_FAILED,
    CN_KOSYNC_PUSH_DNS_FAILED,
    CN_KOSYNC_PUSH_AUTH_FAILED,
    CN_KOSYNC_PUSH_HTTPS_FAILED,
    CN_KOSYNC_PUSH_PROTOCOL_FAILED,
    CN_KOSYNC_PUSH_NO_MEMORY,
    CN_KOSYNC_PUSH_STATUS_COUNT
} cn_kosync_push_status;

typedef struct cn_kosync_push_config {
    const char *document_path;
    cn_progress_store *progress_store;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    const cn_kosync_client *client;
    const char *device;
    const char *device_id; /* required runtime input; never generated */
} cn_kosync_push_config;

/* Only bounded diagnostic evidence; no owned path, XPointer, or credentials.
 * put_invoked means the PUT primitive was called, not that bytes were sent. */
typedef struct cn_kosync_push_result {
    cn_kosync_push_status status;
    cn_koreader_identity_result identity_result;
    cn_progress_result local_load_result;
    cn_time_result time_result;
    cn_dns_result dns_result;
    cn_kosync_result kosync_result;
    cn_kosync_outcome kosync_outcome;
    int put_invoked;
    int remote_uploaded;
    cn_kosync_mutation_state local_mutation;  /* always NONE */
    cn_kosync_mutation_state remote_mutation;
} cn_kosync_push_result;

void cn_kosync_push_result_init(cn_kosync_push_result *result);
cn_kosync_push_status cn_kosync_push_local_once(
    const cn_kosync_push_config *config, cn_kosync_push_result *result);
const char *cn_kosync_push_status_name(cn_kosync_push_status status);

#endif /* CN_SYNC_KOSYNC_PUSH_H */
