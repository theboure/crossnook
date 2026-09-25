/* Additive execution APIs for an already resolved persisted sync profile. */
#ifndef CN_SYNC_PROFILE_SYNC_CONTROLLER_H
#define CN_SYNC_PROFILE_SYNC_CONTROLLER_H

#include "sync/persisted_sync_profile.h"
#include "sync/sync_controller.h"

typedef struct cn_sync_runtime_inputs {
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
} cn_sync_runtime_inputs;

cn_sync_controller_result cn_sync_current_book_with_profile(
    const cn_persisted_sync_profile *profile,
    const cn_sync_runtime_inputs *runtime,
    cn_progress_store *progress_store,
    const char *document_path);

cn_sync_push_result cn_sync_push_local_current_book_with_profile(
    const cn_persisted_sync_profile *profile,
    const cn_sync_runtime_inputs *runtime,
    cn_progress_store *progress_store,
    const char *document_path);

cn_sync_pull_result cn_sync_pull_remote_current_book_with_profile(
    const cn_persisted_sync_profile *profile,
    const cn_sync_runtime_inputs *runtime,
    cn_progress_store *progress_store,
    const char *document_path);

#endif /* CN_SYNC_PROFILE_SYNC_CONTROLLER_H */
