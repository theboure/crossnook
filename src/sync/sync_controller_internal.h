/* Internal resolved-input seam shared by legacy and profile controllers. */
#ifndef CN_SYNC_SYNC_CONTROLLER_INTERNAL_H
#define CN_SYNC_SYNC_CONTROLLER_INTERNAL_H

#include "sync/sync_controller.h"

typedef struct cn_sync_resolved_config {
    const char *base_url;
    const char *device_name;
    const char *username;
    const char *userkey;
    const char *device_id;
    cn_progress_store *progress_store;
    const char *document_path;
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
} cn_sync_resolved_config;

void cn_sync_controller_result_init_internal(
    cn_sync_controller_result *result);
void cn_sync_push_result_init_internal(cn_sync_push_result *result);
void cn_sync_pull_result_init_internal(cn_sync_pull_result *result);

int cn_sync_valid_device_id_internal(const char *text);

void cn_sync_current_book_resolved(
    const cn_sync_resolved_config *config,
    cn_sync_controller_result *result);
void cn_sync_push_local_current_book_resolved(
    const cn_sync_resolved_config *config,
    cn_sync_push_result *result);
void cn_sync_pull_remote_current_book_resolved(
    const cn_sync_resolved_config *config,
    cn_sync_pull_result *result);

#endif /* CN_SYNC_SYNC_CONTROLLER_INTERNAL_H */
