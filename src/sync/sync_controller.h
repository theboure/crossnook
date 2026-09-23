/* Explicit, one-shot composition of published KOSync components. */
#ifndef CN_SYNC_SYNC_CONTROLLER_H
#define CN_SYNC_SYNC_CONTROLLER_H

#include "credentials/credential_store.h"
#include "settings/settings_store.h"
#include "sync/kosync_policy.h"

typedef enum cn_sync_controller_stage {
    CN_SYNC_CONTROLLER_INVALID = 0,
    CN_SYNC_CONTROLLER_DISABLED,
    CN_SYNC_CONTROLLER_SETTINGS_FAILED,
    CN_SYNC_CONTROLLER_CREDENTIALS_FAILED,
    CN_SYNC_CONTROLLER_CONFIG_FAILED,
    CN_SYNC_CONTROLLER_INTEGRATION,
    CN_SYNC_CONTROLLER_CLASSIFICATION_FAILED
} cn_sync_controller_stage;

typedef struct cn_sync_controller_config {
    const cn_settings_store *settings_store;
    const cn_credential_store *credential_store;
    cn_progress_store *progress_store;
    const char *document_path; /* same path used to open the book in Reader */
    const char *device_id;     /* explicit runtime input, never generated */
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
} cn_sync_controller_config;

/* No secrets, document paths, positions, HTTP bodies, or owned pointers. */
typedef struct cn_sync_controller_result {
    cn_sync_controller_stage stage;
    cn_kosync_product_result product;
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    cn_kosync_sync_status sync_status;
    cn_kosync_sync_decision decision;
    cn_progress_result local_load_result;
    cn_progress_result local_save_result;
    cn_time_result time_result;
    cn_dns_result dns_result;
    cn_kosync_result kosync_result;
    cn_netsimple_result transport_result;
    int local_save_attempted;
    int remote_put_attempted;
    int local_saved;
    int remote_uploaded;
    /* Classification failed: attempted operations are conservatively POSSIBLE. */
    int mutation_evidence_conservative;
} cn_sync_controller_result;

cn_sync_controller_result cn_sync_current_book(
    const cn_sync_controller_config *config);
const char *cn_sync_controller_stage_name(cn_sync_controller_stage stage);

#endif /* CN_SYNC_SYNC_CONTROLLER_H */
