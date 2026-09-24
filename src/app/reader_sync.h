/* Application-owned, save-first manual Reader -> ProgressStore -> sync seam. */
#ifndef CN_APP_READER_SYNC_H
#define CN_APP_READER_SYNC_H

#include "sync/sync_controller.h"
#include "ui/ui.h"

typedef enum cn_reader_sync_phase {
    CN_READER_SYNC_INVALID = 0,
    CN_READER_SYNC_NOT_OPEN,
    CN_READER_SYNC_CONFIG_MISMATCH,
    CN_READER_SYNC_IDENTITY_FAILED,
    CN_READER_SYNC_CAPTURE_FAILED,
    CN_READER_SYNC_PRE_SAVE_FAILED,
    CN_READER_SYNC_CONTROLLER_COMPLETE,
    CN_READER_SYNC_IMPORT_RELOAD_FAILED,
    CN_READER_SYNC_IMPORT_APPLY_FAILED,
    CN_READER_SYNC_IMPORT_APPLIED
} cn_reader_sync_phase;

typedef struct cn_reader_sync_config {
    cn_ui *ui;
    cn_progress_store *progress_store;
    const cn_sync_controller_config *controller; /* copied for one call */
} cn_reader_sync_config;

/* All values are bounded enums/flags, no owned pointers or XPointers. */
typedef struct cn_reader_sync_result {
    cn_reader_sync_phase phase;
    cn_progress_result pre_save_result;
    cn_progress_result import_reload_result;
    cn_sync_controller_result controller;
    int pre_save_attempted;
    int controller_invoked;
    int redraw_needed;
    int movement_possible;
    int disk_reader_mismatch_possible;
} cn_reader_sync_result;

cn_reader_sync_result cn_reader_sync_manual_once(
    const cn_reader_sync_config *config);

/* Apply an ALREADY persisted record to the open Reader. No sync, GET/PUT,
 * pre-save, mode selection or network; useful after an independently
 * authorized import and for testing the defensive application branch. */
cn_reader_sync_result cn_reader_sync_apply_persisted(
    cn_ui *ui, cn_progress_store *store);

const char *cn_reader_sync_phase_name(cn_reader_sync_phase phase);

#endif /* CN_APP_READER_SYNC_H */
