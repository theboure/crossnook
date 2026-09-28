#include <stdio.h>
#include <string.h>
#include "app/account_setup_ui.h"
#include "settings/settings_store.h"

static cn_account_setup_report failed_report(cn_account_setup_status status,
                                             cn_account_setup_local_state state)
{
    cn_account_setup_report r;
    memset(&r, 0, sizeof r);
    r.status = status;
    r.local_state = state;
    return r;
}

static cn_ui_account_local ui_local(cn_account_setup_local_state local)
{
    switch (local) {
    case CN_ACCOUNT_SETUP_LOCAL_NO_ACCOUNT: return CN_UI_ACCOUNT_LOCAL_NO_ACCOUNT;
    case CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED: return CN_UI_ACCOUNT_LOCAL_PARTIAL_DISABLED;
    case CN_ACCOUNT_SETUP_LOCAL_ORPHAN_CREDENTIALS: return CN_UI_ACCOUNT_LOCAL_ORPHAN_CREDENTIALS;
    case CN_ACCOUNT_SETUP_LOCAL_COMPLETE_DISABLED: return CN_UI_ACCOUNT_LOCAL_COMPLETE_DISABLED;
    case CN_ACCOUNT_SETUP_LOCAL_ENABLED: return CN_UI_ACCOUNT_LOCAL_ENABLED;
    case CN_ACCOUNT_SETUP_LOCAL_CORRUPT_OR_UNSUPPORTED: return CN_UI_ACCOUNT_LOCAL_CORRUPT;
    case CN_ACCOUNT_SETUP_LOCAL_UNREADABLE: return CN_UI_ACCOUNT_LOCAL_UNREADABLE;
    default: return CN_UI_ACCOUNT_LOCAL_UNKNOWN;
    }
}

static cn_ui_account_result ui_result(cn_account_setup_status status)
{
    switch (status) {
    case CN_ACCOUNT_SETUP_ACTIVATED: return CN_UI_ACCOUNT_RESULT_ACTIVATED;
    case CN_ACCOUNT_SETUP_ALREADY_ENABLED: return CN_UI_ACCOUNT_RESULT_ALREADY_ENABLED;
    case CN_ACCOUNT_SETUP_INVALID_INPUT: return CN_UI_ACCOUNT_RESULT_INVALID_INPUT;
    case CN_ACCOUNT_SETUP_PRECONDITION: return CN_UI_ACCOUNT_RESULT_PRECONDITION;
    case CN_ACCOUNT_SETUP_INCOMPLETE_STATE: return CN_UI_ACCOUNT_RESULT_INCOMPLETE;
    case CN_ACCOUNT_SETUP_CORRUPT_OR_UNSUPPORTED_STATE: return CN_UI_ACCOUNT_RESULT_CORRUPT;
    case CN_ACCOUNT_SETUP_BOOTSTRAP_FAILED: return CN_UI_ACCOUNT_RESULT_BOOTSTRAP_FAILED;
    case CN_ACCOUNT_SETUP_BOOTSTRAP_DURABILITY_UNCERTAIN: return CN_UI_ACCOUNT_RESULT_BOOTSTRAP_UNCERTAIN;
    case CN_ACCOUNT_SETUP_IDENTITY_FAILED: return CN_UI_ACCOUNT_RESULT_IDENTITY_FAILED;
    case CN_ACCOUNT_SETUP_IDENTITY_DURABILITY_UNCERTAIN: return CN_UI_ACCOUNT_RESULT_IDENTITY_UNCERTAIN;
    case CN_ACCOUNT_SETUP_AUTH_REJECTED: return CN_UI_ACCOUNT_RESULT_AUTH_REJECTED;
    case CN_ACCOUNT_SETUP_INFRASTRUCTURE_OR_SERVICE_FAILED: return CN_UI_ACCOUNT_RESULT_INFRASTRUCTURE_FAILED;
    case CN_ACCOUNT_SETUP_ACTIVATION_SAVE_FAILED: return CN_UI_ACCOUNT_RESULT_SAVE_FAILED;
    case CN_ACCOUNT_SETUP_ACTIVATION_DURABILITY_UNCERTAIN: return CN_UI_ACCOUNT_RESULT_ACTIVATION_UNCERTAIN;
    case CN_ACCOUNT_SETUP_ENABLED_PROFILE_UNAVAILABLE: return CN_UI_ACCOUNT_RESULT_PROFILE_UNAVAILABLE;
    case CN_ACCOUNT_SETUP_ACTIVATION_FAILED: return CN_UI_ACCOUNT_RESULT_ACTIVATION_FAILED;
    default: return CN_UI_ACCOUNT_RESULT_OTHER;
    }
}

static void apply_report(cn_ui *ui, const cn_account_setup_report *report)
{
    cn_ui_account_set_result(ui, ui_result(report->status),
                             ui_local(report->local_state));
}

static int verify_again(cn_account_setup_ui *app)
{
    cn_platform_storage_verified now;
    if (cn_platform_storage_verify(&app->candidate, &now, NULL) !=
            CN_PLATFORM_STORAGE_OK || strcmp(now.root, app->verified.root) ||
        strcmp(now.mountpoint, app->verified.mountpoint) ||
        now.device_major != app->verified.device_major ||
        now.device_minor != app->verified.device_minor)
        return 0;
    return 1;
}

static int is_data_path(const char *path)
{
    return path && !strncmp(path, "/data", 5) &&
           (path[5] == '\0' || path[5] == '/');
}

int cn_account_setup_ui_init(cn_account_setup_ui *app,
                             const cn_account_setup_ui_config *config)
{
    int error = 0;
    if (!app || !config || !config->mountpoint || !config->root ||
        is_data_path(config->mountpoint) || is_data_path(config->root) ||
        !config->dns_ip || !config->sntp_ip || !config->ca_path)
        return -1;
    memset(app, 0, sizeof *app);
    app->config = *config;
    app->candidate.root = config->root;
    app->candidate.mountpoint = config->mountpoint;
    app->candidate.expected_major = config->expected_major;
    app->candidate.expected_minor = config->expected_minor;
    if (cn_platform_storage_verify(&app->candidate, &app->verified, &error) !=
            CN_PLATFORM_STORAGE_OK ||
        cn_storage_layout_init(&app->layout, app->verified.root, &error) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(&app->layout, &error) != CN_STORAGE_OK)
        return -1;
    app->dns.servers[0] = config->dns_ip;
    app->dns.server_count = 1;
    app->dns.port = config->dns_port;
    app->tls.ca_path = config->ca_path;
    app->time.servers[0] = config->sntp_ip;
    app->time.server_count = 1;
    app->time.port = config->sntp_port;
    app->runtime.dns = &app->dns;
    app->runtime.tls = &app->tls;
    app->runtime.time_policy = CN_KOSYNC_SYNC_TIME_ESTABLISH;
    app->runtime.time = &app->time;
    app->prepared = 1;
    return 0;
}

static void prefill_partial(cn_account_setup_ui *app, cn_ui *ui)
{
    char path[CN_STORAGE_PATH_CAPACITY];
    cn_settings_store store;
    cn_settings settings;
    if (cn_storage_layout_path(&app->layout, CN_STORAGE_LOCATION_CONFIG,
                               path, sizeof path) != CN_STORAGE_OK ||
        cn_settings_store_init(&store, path, NULL) != CN_SETTINGS_OK ||
        cn_settings_load(&store, &settings, NULL) != CN_SETTINGS_OK)
        return;
    cn_ui_account_prefill(ui, settings.kosync_base_url,
                          settings.kosync_device_name);
    memset(&settings, 0, sizeof settings);
}

int cn_account_setup_ui_process(cn_account_setup_ui *app, cn_ui *ui)
{
    cn_ui_account_action action;
    cn_account_setup_report report;
    cn_account_setup_input input;
    if (!app || !ui || (action = cn_ui_account_take_action(ui)) == CN_UI_ACCOUNT_NONE)
        return 0;
    if (!app->prepared || !verify_again(app)) {
        report = failed_report(CN_ACCOUNT_SETUP_PRECONDITION,
                               CN_ACCOUNT_SETUP_LOCAL_UNREADABLE);
        apply_report(ui, &report);
        return 1;
    }
    switch (action) {
    case CN_UI_ACCOUNT_INSPECT:
    case CN_UI_ACCOUNT_RECHECK:
        report = cn_account_setup_inspect(&app->layout);
        apply_report(ui, &report);
        if (report.local_state == CN_ACCOUNT_SETUP_LOCAL_PARTIAL_DISABLED)
            prefill_partial(app, ui);
        return 1;
    case CN_UI_ACCOUNT_ACTIVATE_EXISTING:
        report = cn_account_setup_activate_existing(&app->layout, &app->runtime);
        apply_report(ui, &report);
        return 1;
    case CN_UI_ACCOUNT_SUBMIT_NEW_OR_RESUME:
    case CN_UI_ACCOUNT_REPLACE_DISABLED:
        {
            cn_ui_account_input view;
            if (cn_ui_account_get_input(ui, &view) != 0) return -1;
            input.base_url = view.base_url;
            input.device_name = view.device_name;
            input.username = view.username;
            input.password = view.password;
            input.password_length = view.password_length;
        }
        report = cn_account_setup_submit(&app->layout, &input,
            action == CN_UI_ACCOUNT_REPLACE_DISABLED
                ? CN_ACCOUNT_SETUP_REPLACE_DISABLED
                : CN_ACCOUNT_SETUP_NEW_OR_RESUME,
            &app->runtime);
        /* The controller borrows the UI password; clear before any formatting
         * or rendering of its report. */
        cn_ui_account_clear_password(ui);
        apply_report(ui, &report);
        return 1;
    default:
        return 0;
    }
}
