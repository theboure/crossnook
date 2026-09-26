#include <string.h>

#include "account/account_setup_controller.h"

typedef enum observation_result {
    OBSERVATION_OK = 0,
    OBSERVATION_CORRUPT,
    OBSERVATION_UNREADABLE
} observation_result;

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)data;
    while (length--)
        *bytes++ = 0;
}

static cn_account_setup_report initial_report(void)
{
    cn_account_setup_report report;
    memset(&report, 0, sizeof report);
    report.status = CN_ACCOUNT_SETUP_INVALID_INPUT;
    report.stage = CN_ACCOUNT_SETUP_STAGE_NONE;
    report.local_state = CN_ACCOUNT_SETUP_LOCAL_UNKNOWN;
    report.bootstrap.result = CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT;
    report.bootstrap.stage = CN_ACCOUNT_BOOTSTRAP_STAGE_NONE;
    report.bootstrap.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    report.bootstrap.credential_result = CN_CREDENTIAL_INVALID;
    report.bootstrap.userkey_result = CN_KOSYNC_USERKEY_INVALID_ARGUMENT;
    report.identity_result = CN_DEVICE_ID_INVALID_ARGUMENT;
    report.activation.status = CN_SYNC_ACTIVATION_INVALID_ARGUMENT;
    report.activation.settings_result = CN_SETTINGS_INVALID_ARGUMENT;
    report.activation.credential_result = CN_CREDENTIAL_INVALID;
    report.activation.identity_result = CN_DEVICE_ID_INVALID_ARGUMENT;
    report.activation.time_result = CN_TIME_INVALID;
    report.activation.dns_result = CN_DNS_INVALID;
    report.activation.kosync_result = CN_KOSYNC_INVALID;
    report.activation.kosync_outcome.transport_result = CN_NETSIMPLE_INVALID;
    report.profile.status = CN_PERSISTED_SYNC_PROFILE_INVALID_ARGUMENT;
    return report;
}

static int bounded_text(const char *text, size_t capacity, int required,
                        size_t *length)
{
    size_t i;
    if (!text)
        return 0;
    for (i = 0; i < capacity; ++i) {
        if (text[i] == '\0') {
            if (length)
                *length = i;
            return !required || i != 0;
        }
    }
    return 0;
}

static int setup_input_basic_valid(const cn_account_setup_input *input)
{
    size_t length;
    if (!input || !input->base_url || !input->device_name ||
        !input->username || !input->password || input->password_length == 0 ||
        input->password_length > CN_ACCOUNT_PASSWORD_MAX)
        return 0;
    if (!bounded_text(input->base_url, CN_SETTINGS_KOSYNC_URL_CAPACITY, 1,
                      &length) || length == 0)
        return 0;
    if (!bounded_text(input->device_name, CN_SETTINGS_DEVICE_NAME_CAPACITY, 1,
                      &length) || length == 0)
        return 0;
    return bounded_text(input->username, CN_CREDENTIAL_USERNAME_MAX + 1, 1,
                        &length);
}

static int state_directory(const cn_storage_layout *layout, char *output,
                           size_t capacity)
{
    char progress[CN_STORAGE_PATH_CAPACITY];
    size_t length;

    if (!layout || !output || capacity == 0 ||
        cn_storage_layout_path(layout, CN_STORAGE_LOCATION_PROGRESS,
                               progress, sizeof progress) != CN_STORAGE_OK)
        return 0;
    length = strlen(progress);
    if (length <= sizeof "/progress" - 1 ||
        strcmp(progress + length - (sizeof "/progress" - 1),
               "/progress") != 0)
        return 0;
    length -= sizeof "/progress" - 1;
    if (length + 1 > capacity)
        return 0;
    memcpy(output, progress, length);
    output[length] = '\0';
    return 1;
}

static int result_is_corrupt_settings(cn_settings_result result)
{
    return result == CN_SETTINGS_CORRUPT ||
           result == CN_SETTINGS_UNSUPPORTED_VERSION;
}

static int result_is_corrupt_credentials(cn_credential_result result)
{
    return result == CN_CREDENTIAL_CORRUPT ||
           result == CN_CREDENTIAL_UNSUPPORTED_VERSION;
}

static int result_is_corrupt_identity(cn_device_identity_result result)
{
    return result == CN_DEVICE_ID_MALFORMED ||
           result == CN_DEVICE_ID_UNSUPPORTED_VERSION ||
           result == CN_DEVICE_ID_BAD_CHECKSUM;
}

static observation_result observe_state(const cn_storage_layout *layout,
                                        cn_account_setup_report *report,
                                        cn_settings *settings)
{
    cn_settings_store settings_store;
    cn_credential_store credential_store;
    cn_credentials credentials;
    char config_path[CN_STORAGE_PATH_CAPACITY];
    char state_path_buffer[CN_STORAGE_PATH_CAPACITY];
    cn_settings_result settings_result;
    cn_credential_result credential_result;
    int settings_missing;

    memset(&settings_store, 0, sizeof settings_store);
    memset(&credential_store, 0, sizeof credential_store);
    memset(&credentials, 0, sizeof credentials);
    if (!layout || !report || !settings ||
        cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG,
                                config_path, sizeof config_path) !=
            CN_STORAGE_OK ||
        !state_directory(layout, state_path_buffer, sizeof state_path_buffer)) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_UNREADABLE;
        return OBSERVATION_UNREADABLE;
    }
    settings_result = cn_settings_store_init(&settings_store, config_path,
                                             &report->system_errno);
    if (settings_result != CN_SETTINGS_OK) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_UNREADABLE;
        return OBSERVATION_UNREADABLE;
    }
    settings_result = cn_settings_load(&settings_store, settings,
                                       &report->system_errno);
    if (result_is_corrupt_settings(settings_result)) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_CORRUPT_OR_UNSUPPORTED;
        return OBSERVATION_CORRUPT;
    }
    if (settings_result != CN_SETTINGS_OK &&
        settings_result != CN_SETTINGS_MISSING) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_UNREADABLE;
        return OBSERVATION_UNREADABLE;
    }
    settings_missing = settings_result == CN_SETTINGS_MISSING;
    if (!settings_missing && settings->kosync_enabled) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_ENABLED;
        return OBSERVATION_OK;
    }
    credential_result = cn_credential_store_init(
        &credential_store, state_path_buffer, &report->system_errno);
    if (credential_result != CN_CREDENTIAL_OK) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_UNREADABLE;
        return OBSERVATION_UNREADABLE;
    }
    credential_result = cn_credential_store_load(
        &credential_store, &credentials, &report->system_errno);
    if (result_is_corrupt_credentials(credential_result)) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_CORRUPT_OR_UNSUPPORTED;
        cn_credentials_clear(&credentials);
        return OBSERVATION_CORRUPT;
    }
    if (credential_result == CN_CREDENTIAL_MISSING) {
        report->local_state = settings_missing
                                  ? CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT
                                  : CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED;
        cn_credentials_clear(&credentials);
        return OBSERVATION_OK;
    }
    if (credential_result != CN_CREDENTIAL_OK) {
        report->local_state = CN_ACCOUNT_SETUP_LOCAL_UNREADABLE;
        cn_credentials_clear(&credentials);
        return OBSERVATION_UNREADABLE;
    }
    report->local_state = settings_missing
                              ? CN_ACCOUNT_SETUP_LOCAL_ORPHAN_CREDENTIALS
                              : CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED;
    cn_credentials_clear(&credentials);
    return OBSERVATION_OK;
}

static int input_matches_disabled_settings(const cn_account_setup_input *input,
                                           const cn_settings *settings)
{
    return input && settings &&
           !strcmp(input->base_url, settings->kosync_base_url) &&
           !strcmp(input->device_name, settings->kosync_device_name);
}

static cn_account_setup_status bootstrap_status(
    const cn_account_bootstrap_report *bootstrap)
{
    if (bootstrap->result == CN_ACCOUNT_BOOTSTRAP_DURABILITY_UNCERTAIN)
        return CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN;
    if (bootstrap->result == CN_ACCOUNT_BOOTSTRAP_INVALID_INPUT ||
        bootstrap->result == CN_ACCOUNT_BOOTSTRAP_INVALID_ARGUMENT ||
        bootstrap->result == CN_ACCOUNT_BOOTSTRAP_DERIVATION_FAILED)
        return CN_ACCOUNT_SETUP_INVALID_INPUT;
    return CN_ACCOUNT_SETUP_BOOTSTRAP_FAILED;
}

static cn_account_setup_status activation_status(
    const cn_sync_activation_report *activation)
{
    switch (activation->status) {
    case CN_SYNC_ACTIVATION_ALREADY_ENABLED:
        return CN_ACCOUNT_SETUP_ALREADY_ENABLED;
    case CN_SYNC_ACTIVATION_OK:
        return CN_ACCOUNT_SETUP_ACTIVATED;
    case CN_SYNC_ACTIVATION_AUTH_REJECTED:
        return CN_ACCOUNT_SETUP_AUTH_REJECTED;
    case CN_SYNC_ACTIVATION_TRUSTED_TIME_FAILED:
    case CN_SYNC_ACTIVATION_DNS_FAILED:
    case CN_SYNC_ACTIVATION_HTTPS_FAILED:
    case CN_SYNC_ACTIVATION_SERVICE_FAILED:
        return CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED;
    case CN_SYNC_ACTIVATION_SETTINGS_SAVE_FAILED:
        return CN_ACCOUNT_SETUP_ACTIVATION_SAVE_FAILED;
    case CN_SYNC_ACTIVATION_DURABILITY_UNCERTAIN:
        return CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN;
    case CN_SYNC_ACTIVATION_SETTINGS_FAILED:
    case CN_SYNC_ACTIVATION_CREDENTIALS_FAILED:
    case CN_SYNC_ACTIVATION_IDENTITY_FAILED:
        return CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE;
    case CN_SYNC_ACTIVATION_CONFIG_FAILED:
        return CN_ACCOUNT_SETUP_PRECONDITION;
    default:
        return CN_ACCOUNT_SETUP_ACTIVATION_FAILED;
    }
}

static cn_account_setup_report finish_activation(
    cn_account_setup_report report, const cn_storage_layout *layout,
    const cn_sync_activation_runtime *runtime)
{
    cn_persisted_sync_profile profile;
    cn_account_setup_status status;

    report.stage = CN_ACCOUNT_SETUP_STAGE_ACTIVATION;
    report.activation_attempted = 1;
    report.activation = cn_sync_activate(layout, runtime);
    if (report.activation.system_errno)
        report.system_errno = report.activation.system_errno;
    status = activation_status(&report.activation);
    if (status != CN_ACCOUNT_SETUP_ACTIVATED) {
        report.status = status;
        return report;
    }
    report.stage = CN_ACCOUNT_SETUP_STAGE_PROFILE_CHECK;
    report.profile_check_attempted = 1;
    memset(&profile, 0, sizeof profile);
    report.profile = cn_persisted_sync_profile_load(layout, &profile);
    if (report.profile.system_errno)
        report.system_errno = report.profile.system_errno;
    cn_persisted_sync_profile_clear(&profile);
    if (report.profile.status != CN_PERSISTED_SYNC_PROFILE_READY) {
        report.status = CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE;
        return report;
    }
    report.status = CN_ACCOUNT_SETUP_ACTIVATED;
    return report;
}

static cn_account_setup_report prepare_and_activate(
    cn_account_setup_report report, const cn_storage_layout *layout,
    const cn_sync_activation_runtime *runtime)
{
    cn_device_identity_store identity_store;
    char device_id[CN_DEVICE_ID_TEXT_CAPACITY];
    cn_device_identity_result result;

    memset(&identity_store, 0, sizeof identity_store);
    memset(device_id, 0, sizeof device_id);
    report.stage = CN_ACCOUNT_SETUP_STAGE_IDENTITY;
    report.identity_attempted = 1;
    result = cn_device_identity_store_init(&identity_store, layout, NULL, NULL);
    report.identity_result = result;
    if (result != CN_DEVICE_ID_OK) {
        report.status = result_is_corrupt_identity(result)
                            ? CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE
                            : CN_ACCOUNT_SETUP_IDENTITY_FAILED;
        clear_bytes(device_id, sizeof device_id);
        return report;
    }
    result = cn_device_identity_load(&identity_store, device_id,
                                     sizeof device_id, &report.system_errno);
    report.identity_result = result;
    if (result == CN_DEVICE_ID_NOT_FOUND) {
        result = cn_device_identity_load_or_create(
            &identity_store, device_id, sizeof device_id, &report.system_errno);
        report.identity_result = result;
    }
    if (result == CN_DEVICE_ID_DURABILITY_UNCERTAIN) {
        report.status = CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN;
        clear_bytes(device_id, sizeof device_id);
        return report;
    }
    if (result != CN_DEVICE_ID_OK && result != CN_DEVICE_ID_CREATED) {
        report.status = result_is_corrupt_identity(result)
                            ? CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE
                            : CN_ACCOUNT_SETUP_IDENTITY_FAILED;
        clear_bytes(device_id, sizeof device_id);
        return report;
    }
    clear_bytes(device_id, sizeof device_id);
    return finish_activation(report, layout, runtime);
}

static cn_account_setup_report run_setup(
    cn_account_setup_report report, const cn_storage_layout *layout,
    const cn_account_setup_input *input, cn_account_setup_intent intent,
    const cn_sync_activation_runtime *runtime, const cn_settings *settings)
{
    if (intent == CN_ACCOUNT_SETUP_NEW_OR_RESUME &&
        report.local_state == CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED) {
        report.status = CN_ACCOUNT_SETUP_PRECONDITION;
        return report;
    }
    if (intent == CN_ACCOUNT_SETUP_REPLACE_DISABLED &&
        report.local_state != CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED) {
        report.status = CN_ACCOUNT_SETUP_PRECONDITION;
        return report;
    }
    if (intent != CN_ACCOUNT_SETUP_NEW_OR_RESUME &&
        intent != CN_ACCOUNT_SETUP_REPLACE_DISABLED) {
        report.status = CN_ACCOUNT_SETUP_INVALID_INPUT;
        return report;
    }
    if (!setup_input_basic_valid(input)) {
        report.status = CN_ACCOUNT_SETUP_INVALID_INPUT;
        return report;
    }
    if (report.local_state == CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED &&
        !input_matches_disabled_settings(input, settings)) {
        report.status = CN_ACCOUNT_SETUP_PRECONDITION;
        return report;
    }
    report.stage = CN_ACCOUNT_SETUP_STAGE_BOOTSTRAP;
    report.bootstrap_attempted = 1;
    report.bootstrap = cn_account_bootstrap(
        layout, input->base_url, input->device_name, input->username,
        input->password, input->password_length, &report.system_errno);
    if (report.bootstrap.result != CN_ACCOUNT_BOOTSTRAP_OK) {
        report.status = report.bootstrap.result == CN_ACCOUNT_BOOTSTRAP_EXISTING_ACTIVE
                            ? CN_ACCOUNT_SETUP_ALREADY_ENABLED
                            : bootstrap_status(&report.bootstrap);
        return report;
    }
    return prepare_and_activate(report, layout, runtime);
}

cn_account_setup_report cn_account_setup_submit(
    const cn_storage_layout *prepared_layout,
    const cn_account_setup_input *input, cn_account_setup_intent intent,
    const cn_sync_activation_runtime *runtime)
{
    cn_account_setup_report report;
    cn_settings settings;
    observation_result result;

    memset(&settings, 0, sizeof settings);
    report = initial_report();
    report.stage = CN_ACCOUNT_SETUP_STAGE_PREFLIGHT;
    result = observe_state(prepared_layout, &report, &settings);
    if (result != OBSERVATION_OK) {
        report.status = result == OBSERVATION_CORRUPT
                            ? CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE
                            : CN_ACCOUNT_SETUP_PRECONDITION;
        clear_bytes(&settings, sizeof settings);
        return report;
    }
    if (report.local_state == CN_ACCOUNT_SETUP_LOCAL_ORPHAN_CREDENTIALS) {
        report.status = CN_ACCOUNT_SETUP_INCOMPLETE_STATE;
        clear_bytes(&settings, sizeof settings);
        return report;
    }
    if (report.local_state == CN_ACCOUNT_SETUP_LOCAL_ENABLED) {
        report.status = CN_ACCOUNT_SETUP_ALREADY_ENABLED;
        clear_bytes(&settings, sizeof settings);
        return report;
    }
    report = run_setup(report, prepared_layout, input, intent, runtime, &settings);
    clear_bytes(&settings, sizeof settings);
    return report;
}

cn_account_setup_report cn_account_setup_activate_existing(
    const cn_storage_layout *prepared_layout,
    const cn_sync_activation_runtime *runtime)
{
    cn_account_setup_report report;
    observation_result result;
    cn_settings settings;

    memset(&settings, 0, sizeof settings);
    report = initial_report();
    report.stage = CN_ACCOUNT_SETUP_STAGE_PREFLIGHT;
    result = observe_state(prepared_layout, &report, &settings);
    clear_bytes(&settings, sizeof settings);
    if (result == OBSERVATION_CORRUPT) {
        report.status = CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE;
        return report;
    }
    if (result == OBSERVATION_UNREADABLE) {
        report.status = CN_ACCOUNT_SETUP_PRECONDITION;
        return report;
    }
    if (report.local_state == CN_ACCOUNT_SETUP_LOCAL_ENABLED) {
        report.status = CN_ACCOUNT_SETUP_ALREADY_ENABLED;
        return report;
    }
    if (report.local_state != CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED) {
        report.status = CN_ACCOUNT_SETUP_INCOMPLETE_STATE;
        return report;
    }
    return prepare_and_activate(report, prepared_layout, runtime);
}

const char *cn_account_setup_status_name(cn_account_setup_status status)
{
    static const char *const names[CN_ACCOUNT_SETUP_STATUS_COUNT] = {
        "activated", "already-enabled", "invalid-input", "precondition",
        "incomplete-state", "corrupt-or-unsupported-state", "bootstrap-failed",
        "bootstrap-durability-uncertain", "identity-failed",
        "identity-durability-uncertain", "auth-rejected",
        "infrastructure-or-service-failed", "activation-save-failed",
        "activation-durability-uncertain", "enabled-profile-unavailable",
        "activation-failed"
    };
    return status >= 0 && status < CN_ACCOUNT_SETUP_STATUS_COUNT
               ? names[status] : "unknown";
}

const char *cn_account_setup_stage_name(cn_account_setup_stage stage)
{
    static const char *const names[CN_ACCOUNT_SETUP_STAGE_COUNT] = {
        "none", "preflight", "bootstrap", "identity", "activation",
        "profile-check"
    };
    return stage >= 0 && stage < CN_ACCOUNT_SETUP_STAGE_COUNT
               ? names[stage] : "unknown";
}

const char *cn_account_setup_local_state_name(cn_account_setup_local_state state)
{
    static const char *const names[CN_ACCOUNT_SETUP_LOCAL_STATE_COUNT] = {
        "unknown", "no-account", "partial-disabled", "orphan-credentials",
        "complete-disabled", "enabled", "corrupt-or-unsupported", "unreadable"
    };
    return state >= 0 && state < CN_ACCOUNT_SETUP_LOCAL_STATE_COUNT
               ? names[state] : "unknown";
}
