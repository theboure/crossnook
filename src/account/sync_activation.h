/* Explicit remote-authenticated activation of a persisted sync account. */
#ifndef CN_ACCOUNT_SYNC_ACTIVATION_H
#define CN_ACCOUNT_SYNC_ACTIVATION_H

#include "credentials/credential_store.h"
#include "identity/device_identity.h"
#include "net/dnssimple.h"
#include "net/netsimple.h"
#include "net/tlssimple.h"
#include "settings/settings_store.h"
#include "storage/storage_layout.h"
#include "sync/kosync.h"
#include "sync/kosync_sync.h"
#include "time/timesimple.h"

typedef struct cn_sync_activation_runtime {
    const cn_dns_config *dns;
    const cn_tls_config *tls;
    cn_kosync_sync_time_policy time_policy;
    const cn_time_config *time;
} cn_sync_activation_runtime;

typedef enum cn_sync_activation_status {
    CN_SYNC_ACTIVATION_OK = 0,
    CN_SYNC_ACTIVATION_ALREADY_ENABLED,
    CN_SYNC_ACTIVATION_INVALID_ARGUMENT,
    CN_SYNC_ACTIVATION_SETTINGS_FAILED,
    CN_SYNC_ACTIVATION_CREDENTIALS_FAILED,
    CN_SYNC_ACTIVATION_IDENTITY_FAILED,
    CN_SYNC_ACTIVATION_CONFIG_FAILED,
    CN_SYNC_ACTIVATION_TRUSTED_TIME_FAILED,
    CN_SYNC_ACTIVATION_DNS_FAILED,
    CN_SYNC_ACTIVATION_AUTH_REJECTED,
    CN_SYNC_ACTIVATION_HTTPS_FAILED,
    CN_SYNC_ACTIVATION_SERVICE_FAILED,
    CN_SYNC_ACTIVATION_SETTINGS_SAVE_FAILED,
    CN_SYNC_ACTIVATION_DURABILITY_UNCERTAIN,
    CN_SYNC_ACTIVATION_NO_MEMORY,
    CN_SYNC_ACTIVATION_STATUS_COUNT
} cn_sync_activation_status;

/* Contains status and component results only; never account material. */
typedef struct cn_sync_activation_report {
    cn_sync_activation_status status;
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    cn_device_identity_result identity_result;
    cn_time_result time_result;
    cn_dns_result dns_result;
    cn_kosync_result kosync_result;
    cn_kosync_outcome kosync_outcome;
    int settings_attempted;
    int credentials_attempted;
    int identity_attempted;
    int time_attempted;
    int dns_attempted;
    int remote_auth_attempted;
    int remote_auth_succeeded;
    int settings_save_attempted;
    int settings_enable_visible_possible;
    int system_errno;
} cn_sync_activation_report;

cn_sync_activation_report cn_sync_activate(
    const cn_storage_layout *prepared_layout,
    const cn_sync_activation_runtime *runtime);

const char *cn_sync_activation_status_name(cn_sync_activation_status status);

#endif /* CN_ACCOUNT_SYNC_ACTIVATION_H */
