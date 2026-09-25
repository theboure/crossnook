/* Local-only KOSync account/configuration bootstrap. */
#ifndef CN_ACCOUNT_ACCOUNT_BOOTSTRAP_H
#define CN_ACCOUNT_ACCOUNT_BOOTSTRAP_H

#include "account/kosync_userkey.h"
#include "credentials/credential_store.h"
#include "settings/settings_store.h"
#include "storage/storage_layout.h"

typedef enum cn_account_bootstrap_stage {
    CN_ACCOUNT_BOOTSTRAP_STAGE_NONE = 0,
    CN_ACCOUNT_BOOTSTRAP_STAGE_PREFLIGHT,
    CN_ACCOUNT_BOOTSTRAP_STAGE_SETTINGS,
    CN_ACCOUNT_BOOTSTRAP_STAGE_CREDENTIALS
} cn_account_bootstrap_stage;

typedef enum cn_account_bootstrap_result {
    CN_ACCOUNT_BOOTSTRAP_OK = 0,
    CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT,
    CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT,
    CN_ACCOUNT_BOOTSTRAP_EXISTING_ACTIVE,
    CN_ACCOUNT_BOOTSTRAP_EXISTING_SETTINGS_FAILED,
    CN_ACCOUNT_BOOTSTRAP_EXISTING_CREDENTIALS_FAILED,
    CN_ACCOUNT_BOOTSTRAP_DERIVATION_FAILED,
    CN_ACCOUNT_BOOTSTRAP_SETTINGS_WRITE_FAILED,
    CN_ACCOUNT_BOOTSTRAP_CREDENTIALS_WRITE_FAILED,
    CN_ACCOUNT_BOOTSTRAP_DURABILITY_UNCERTAIN,
    CN_ACCOUNT_BOOTSTRAP_RESULT_COUNT
} cn_account_bootstrap_result;

/* Contains status only; never contains username, password, or userkey data. */
typedef struct cn_account_bootstrap_report {
    cn_account_bootstrap_result result;
    cn_account_bootstrap_stage stage;
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    cn_kosync_userkey_result userkey_result;
    int settings_written;
    int credentials_written;
} cn_account_bootstrap_report;

/* The layout must already be initialized and prepared by the caller. */
cn_account_bootstrap_report cn_account_bootstrap(
    const cn_storage_layout *layout,
    const char *base_url,
    const char *device_name,
    const char *username,
    const unsigned char *password,
    size_t password_length,
    int *system_errno);

const char *cn_account_bootstrap_result_name(cn_account_bootstrap_result result);
const char *cn_account_bootstrap_stage_name(cn_account_bootstrap_stage stage);

#endif /* CN_ACCOUNT_ACCOUNT_BOOTSTRAP_H */
