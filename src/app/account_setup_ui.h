#ifndef CN_APP_ACCOUNT_SETUP_UI_H
#define CN_APP_ACCOUNT_SETUP_UI_H

#include "account/account_setup_controller.h"
#include "platform/storage_verify.h"
#include "ui/ui.h"

typedef struct cn_account_setup_ui_config {
    const char *mountpoint;
    const char *root;
    unsigned expected_major;
    unsigned expected_minor;
    const char *dns_ip;
    unsigned dns_port;
    const char *sntp_ip;
    unsigned sntp_port;
    const char *ca_path;
} cn_account_setup_ui_config;

typedef struct cn_account_setup_ui {
    cn_account_setup_ui_config config;
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_dns_config dns;
    cn_tls_config tls;
    cn_time_config time;
    cn_sync_activation_runtime runtime;
    int prepared;
} cn_account_setup_ui;

int cn_account_setup_ui_init(cn_account_setup_ui *app,
                             const cn_account_setup_ui_config *config);
/* Consume one UI action, call the controller outside UI/rendering, and apply
 * only its bounded secret-free report to the screen. 1 means work occurred. */
int cn_account_setup_ui_process(cn_account_setup_ui *app, cn_ui *ui);

#endif
