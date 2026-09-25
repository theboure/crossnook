/* Read-only composition of persisted sync account state. */
#ifndef CN_SYNC_PERSISTED_SYNC_PROFILE_H
#define CN_SYNC_PERSISTED_SYNC_PROFILE_H

#include "credentials/credential_store.h"
#include "identity/device_identity.h"
#include "settings/settings_store.h"
#include "storage/storage_layout.h"

typedef struct cn_persisted_sync_profile {
    char base_url[CN_SETTINGS_KOSYNC_URL_CAPACITY];
    char username[CN_CREDENTIAL_USERNAME_MAX + 1];
    char userkey[CN_CREDENTIAL_USERKEY_MAX + 1];
    char device_id[CN_DEVICE_ID_TEXT_CAPACITY];
    char device_name[CN_SETTINGS_DEVICE_NAME_CAPACITY];
} cn_persisted_sync_profile;

typedef enum cn_persisted_sync_profile_status {
    CN_PERSISTED_SYNC_PROFILE_INVALID_ARGUMENT = 0,
    CN_PERSISTED_SYNC_PROFILE_DISABLED,
    CN_PERSISTED_SYNC_PROFILE_READY,
    CN_PERSISTED_SYNC_PROFILE_SETTINGS_FAILED,
    CN_PERSISTED_SYNC_PROFILE_CREDENTIALS_FAILED,
    CN_PERSISTED_SYNC_PROFILE_IDENTITY_FAILED,
    CN_PERSISTED_SYNC_PROFILE_VALIDATION_FAILED,
    CN_PERSISTED_SYNC_PROFILE_STATUS_COUNT
} cn_persisted_sync_profile_status;

typedef struct cn_persisted_sync_profile_report {
    cn_persisted_sync_profile_status status;
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    cn_device_identity_result identity_result;
    int settings_attempted;
    int credentials_attempted;
    int identity_attempted;
    int system_errno;
} cn_persisted_sync_profile_report;

void cn_persisted_sync_profile_clear(cn_persisted_sync_profile *profile);

cn_persisted_sync_profile_report cn_persisted_sync_profile_load(
    const cn_storage_layout *prepared_layout,
    cn_persisted_sync_profile *output);

const char *cn_persisted_sync_profile_status_name(
    cn_persisted_sync_profile_status status);

#endif /* CN_SYNC_PERSISTED_SYNC_PROFILE_H */
