/* Application-facing orchestration for first-time KOSync account setup. */
#ifndef CN_ACCOUNT_ACCOUNT_SETUP_CONTROLLER_H
#define CN_ACCOUNT_ACCOUNT_SETUP_CONTROLLER_H

#include "account/account_bootstrap.h"
#include "account/sync_activation.h"
#include "identity/device_identity.h"
#include "storage/storage_layout.h"
#include "sync/persisted_sync_profile.h"

typedef struct cn_account_setup_input {
    const char *base_url;
    const char *device_name;
    const char *username;
    const unsigned char *password;
    size_t password_length;
} cn_account_setup_input;

typedef enum cn_account_setup_intent {
    CN_ACCOUNT_SETUP_NEW_OR_RESUME = 0,
    CN_ACCOUNT_SETUP_REPLACE_DISABLED,
    CN_ACCOUNT_SETUP_INTENT_COUNT
} cn_account_setup_intent;

typedef enum cn_account_setup_stage {
    CN_ACCOUNT_SETUP_STAGE_NONE = 0,
    CN_ACCOUNT_SETUP_STAGE_PREFLIGHT,
    CN_ACCOUNT_SETUP_STAGE_BOOTSTRAP,
    CN_ACCOUNT_SETUP_STAGE_IDENTITY,
    CN_ACCOUNT_SETUP_STAGE_ACTIVATION,
    CN_ACCOUNT_SETUP_STAGE_PROFILE_CHECK,
    CN_ACCOUNT_SETUP_STAGE_COUNT
} cn_account_setup_stage;

typedef enum cn_account_setup_local_state {
    CN_ACCOUNT_SETUP_LOCAL_UNKNOWN = 0,
    CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT,
    CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED,
    CN_ACCOUNT_SETUP_LOCAL_ORPHAN_CREDENTIALS,
    CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED,
    CN_ACCOUNT_SETUP_LOCAL_ENABLED,
    CN_ACCOUNT_SETUP_LOCAL_CORRUPT_OR_UNSUPPORTED,
    CN_ACCOUNT_SETUP_LOCAL_UNREADABLE,
    CN_ACCOUNT_SETUP_LOCAL_STATE_COUNT
} cn_account_setup_local_state;

typedef enum cn_account_setup_status {
    CN_ACCOUNT_SETUP_ACTIVATED = 0,
    CN_ACCOUNT_SETUP_ALREADY_ENABLED,
    CN_ACCOUNT_SETUP_INVALID_INPUT,
    CN_ACCOUNT_SETUP_PRECONDITION,
    CN_ACCOUNT_SETUP_INCOMPLETE_STATE,
    CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE,
    CN_ACCOUNT_SETUP_BOOTSTRAP_FAILED,
    CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN,
    CN_ACCOUNT_SETUP_IDENTITY_FAILED,
    CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN,
    CN_ACCOUNT_SETUP_AUTH_REJECTED,
    CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED,
    CN_ACCOUNT_SETUP_ACTIVATION_SAVE_FAILED,
    CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN,
    CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE,
    CN_ACCOUNT_SETUP_ACTIVATION_FAILED,
    CN_ACCOUNT_SETUP_STATUS_COUNT
} cn_account_setup_status;

/* Contains status and component results only; never account material. */
typedef struct cn_account_setup_report {
    cn_account_setup_status status;
    cn_account_setup_stage stage;
    cn_account_setup_local_state local_state;
    int bootstrap_attempted;
    cn_account_bootstrap_report bootstrap;
    int identity_attempted;
    cn_device_identity_result identity_result;
    int activation_attempted;
    cn_sync_activation_report activation;
    int profile_check_attempted;
    cn_persisted_sync_profile_report profile;
    int system_errno;
} cn_account_setup_report;

cn_account_setup_report cn_account_setup_submit(
    const cn_storage_layout *prepared_layout,
    const cn_account_setup_input *input,
    cn_account_setup_intent intent,
    const cn_sync_activation_runtime *runtime);

cn_account_setup_report cn_account_setup_activate_existing(
    const cn_storage_layout *prepared_layout,
    const cn_sync_activation_runtime *runtime);

const char *cn_account_setup_status_name(cn_account_setup_status status);
const char *cn_account_setup_stage_name(cn_account_setup_stage stage);
const char *cn_account_setup_local_state_name(cn_account_setup_local_state state);

#endif /* CN_ACCOUNT_ACCOUNT_SETUP_CONTROLLER_H */
