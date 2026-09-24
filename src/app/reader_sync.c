#include <string.h>

#include "app/reader_sync.h"
#include "progress/book_identity.h"

static cn_reader_sync_result empty_result(void)
{
    cn_reader_sync_result result;
    memset(&result, 0, sizeof result);
    result.phase = CN_READER_SYNC_INVALID;
    result.pre_save_result = CN_PROGRESS_INVALID;
    result.import_reload_result = CN_PROGRESS_INVALID;
    result.controller.stage = CN_SYNC_CONTROLLER_INVALID;
    result.controller.product.outcome = CN_KOSYNC_PRODUCT_INTERNAL_FAILURE;
    result.controller.product.retry = CN_KOSYNC_RETRY_EXPLICIT_ACTION;
    return result;
}

static const cn_book *open_book(cn_ui *ui)
{
    const cn_book *book;
    if (!ui || cn_ui_get_state(ui) != CN_UI_READER ||
        cn_ui_reader_pages(ui) < 1)
        return NULL;
    book = cn_ui_selected_book(ui);
    return book && book->format == CN_BOOK_EPUB && book->path &&
           book->path[0] ? book : NULL;
}

static void apply_record(cn_reader_sync_result *result, cn_ui *ui,
                         cn_progress_store *store,
                         const cn_book_identity *identity)
{
    cn_progress_record loaded;
    cn_progress_record_init(&loaded);
    result->import_reload_result = cn_progress_store_load(store, identity,
                                                            &loaded);
    if (result->import_reload_result != CN_PROGRESS_OK) {
        result->phase = CN_READER_SYNC_IMPORT_RELOAD_FAILED;
        result->disk_reader_mismatch_possible = 1;
    } else if (cn_ui_reader_goto_position(ui, &loaded.position) != 0) {
        /* Reader rejects before moving; the imported record can remain B. */
        result->phase = CN_READER_SYNC_IMPORT_APPLY_FAILED;
        result->disk_reader_mismatch_possible = 1;
    } else {
        /* Goto changes in-memory Reader state, not pixels or persisted data.
         * Bookmark recapture need not equal arbitrary mid-page tokens. */
        result->phase = CN_READER_SYNC_IMPORT_APPLIED;
        result->movement_possible = 1;
        result->redraw_needed = 1;
    }
    cn_progress_record_clear(&loaded);
}

cn_reader_sync_result cn_reader_sync_apply_persisted(
    cn_ui *ui, cn_progress_store *store)
{
    cn_reader_sync_result result = empty_result();
    cn_book_identity identity;
    const cn_book *book = open_book(ui);
    if (!book) {
        result.phase = CN_READER_SYNC_NOT_OPEN;
        return result;
    }
    if (!store || cn_book_identity_from_path(&identity, book->path) != 0) {
        result.phase = store ? CN_READER_SYNC_IDENTITY_FAILED
                             : CN_READER_SYNC_INVALID;
        return result;
    }
    apply_record(&result, ui, store, &identity);
    return result;
}

cn_reader_sync_result cn_reader_sync_manual_once(
    const cn_reader_sync_config *config)
{
    cn_reader_sync_result result = empty_result();
    cn_sync_controller_config controller;
    cn_book_identity identity;
    cn_progress_record captured;
    const cn_book *book;

    if (!config || !config->ui || !config->progress_store ||
        !config->controller)
        return result;
    book = open_book(config->ui);
    if (!book) {
        result.phase = CN_READER_SYNC_NOT_OPEN;
        return result;
    }
    if ((config->controller->document_path &&
         strcmp(config->controller->document_path, book->path) != 0) ||
        (config->controller->progress_store &&
         config->controller->progress_store != config->progress_store)) {
        result.phase = CN_READER_SYNC_CONFIG_MISMATCH;
        return result;
    }
    if (cn_book_identity_from_path(&identity, book->path) != 0) {
        result.phase = CN_READER_SYNC_IDENTITY_FAILED;
        return result;
    }
    cn_progress_record_init(&captured);
    if (cn_ui_reader_get_position(config->ui, &captured.position) != 0) {
        result.phase = CN_READER_SYNC_CAPTURE_FAILED;
        cn_progress_record_clear(&captured);
        return result;
    }
    result.pre_save_attempted = 1;
    result.pre_save_result = cn_progress_store_save(config->progress_store,
                                                    &identity, &captured);
    cn_progress_record_clear(&captured);
    if (result.pre_save_result != CN_PROGRESS_OK) {
        result.phase = CN_READER_SYNC_PRE_SAVE_FAILED;
        return result;
    }
    controller = *config->controller;
    controller.document_path = book->path;
    controller.progress_store = config->progress_store;
    result.controller_invoked = 1;
    result.controller = cn_sync_current_book(&controller);
    result.phase = CN_READER_SYNC_CONTROLLER_COMPLETE;
    if (result.controller.stage == CN_SYNC_CONTROLLER_INTEGRATION &&
        result.controller.product.outcome == CN_KOSYNC_PRODUCT_IMPORTED)
        apply_record(&result, config->ui, config->progress_store, &identity);
    return result;
}

const char *cn_reader_sync_phase_name(cn_reader_sync_phase phase)
{
    static const char *const names[] = {
        "invalid", "not-open", "config-mismatch", "identity-failed",
        "capture-failed", "pre-save-failed", "controller-complete",
        "import-reload-failed", "import-apply-failed", "import-applied"
    };
    return phase >= CN_READER_SYNC_INVALID && phase <= CN_READER_SYNC_IMPORT_APPLIED
               ? names[phase] : "unknown";
}
