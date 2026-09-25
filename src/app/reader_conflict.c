#include <string.h>

#include "app/reader_conflict.h"
#include "progress/book_identity.h"

int cn_reader_sync_is_conflict(const cn_reader_sync_result *original)
{
    return original &&
           original->phase == CN_READER_SYNC_CONTROLLER_COMPLETE &&
           original->pre_save_attempted &&
           original->pre_save_result == CN_PROGRESS_OK &&
           original->controller_invoked &&
           original->controller.stage == CN_SYNC_CONTROLLER_INTEGRATION &&
           original->controller.sync_status == CN_KOSYNC_SYNC_STATUS_OK &&
           original->controller.decision == CN_KOSYNC_SYNC_AMBIGUOUS &&
           original->controller.product.outcome == CN_KOSYNC_PRODUCT_CONFLICT;
}

cn_reader_conflict_result cn_reader_sync_resolve_conflict(
    const cn_reader_sync_config *config,
    const cn_reader_sync_result *original,
    const cn_reader_position *expected_position,
    const char *expected_book_path,
    cn_reader_conflict_choice choice)
{
    cn_reader_conflict_result result;
    cn_reader_position current;
    cn_progress_record local;
    cn_book_identity identity;
    cn_sync_controller_config controller;
    const cn_book *book;
    int same;

    memset(&result, 0, sizeof result);
    result.phase = CN_READER_CONFLICT_INVALID;
    result.local_check_result = CN_PROGRESS_INVALID;
    result.push.stage = CN_SYNC_PUSH_INVALID;
    result.pull.stage = CN_SYNC_PULL_INVALID;
    result.reader_apply.phase = CN_READER_SYNC_INVALID;
    if (choice == CN_READER_CONFLICT_CANCEL) {
        result.phase = CN_READER_CONFLICT_CANCELLED;
        return result;
    }
    if ((choice != CN_READER_CONFLICT_USE_LOCAL &&
         choice != CN_READER_CONFLICT_USE_REMOTE) ||
        !cn_reader_sync_is_conflict(original) || !config || !config->ui ||
        !config->progress_store || !config->controller || !expected_position ||
        !expected_position->location || !expected_book_path ||
        !expected_book_path[0]) return result;

    book = cn_ui_selected_book(config->ui);
    if (cn_ui_get_state(config->ui) != CN_UI_READER ||
        cn_ui_reader_pages(config->ui) < 1 || !book ||
        book->format != CN_BOOK_EPUB || !book->path ||
        strcmp(book->path, expected_book_path) ||
        (config->controller->document_path &&
         strcmp(config->controller->document_path, expected_book_path)) ||
        (config->controller->progress_store &&
         config->controller->progress_store != config->progress_store) ||
        cn_book_identity_from_path(&identity, expected_book_path) != 0) {
        result.phase = CN_READER_CONFLICT_STALE;
        return result;
    }

    cn_reader_position_init(&current);
    cn_progress_record_init(&local);
    same = cn_ui_reader_get_position(config->ui, &current) == 0 &&
           current.location &&
           !strcmp(current.location, expected_position->location) &&
           current.progress_10000 == expected_position->progress_10000;
    if (same) {
        result.local_check_result = cn_progress_store_load(
            config->progress_store, &identity, &local);
        same = result.local_check_result == CN_PROGRESS_OK &&
               local.position.location &&
               !strcmp(local.position.location, expected_position->location) &&
               local.position.progress_10000 == expected_position->progress_10000;
    }
    cn_reader_position_clear(&current);
    cn_progress_record_clear(&local);
    if (!same) {
        result.phase = CN_READER_CONFLICT_STALE;
        return result;
    }

    controller = *config->controller;
    controller.document_path = expected_book_path;
    controller.progress_store = config->progress_store;
    if (choice == CN_READER_CONFLICT_USE_LOCAL) {
        result.push_invoked = 1;
        result.push = cn_sync_push_local_current_book(&controller);
        result.phase = CN_READER_CONFLICT_LOCAL_COMPLETE;
        return result;
    }
    result.pull_invoked = 1;
    result.pull = cn_sync_pull_remote_current_book(&controller);
    result.phase = CN_READER_CONFLICT_REMOTE_COMPLETE;
    if (result.pull.stage != CN_SYNC_PULL_EXECUTED ||
        result.pull.outcome != CN_SYNC_PULL_PERSISTED_OUTCOME) return result;
    result.reader_apply = cn_reader_sync_apply_persisted(
        config->ui, config->progress_store);
    if (result.reader_apply.phase != CN_READER_SYNC_IMPORT_APPLIED) {
        result.phase = CN_READER_CONFLICT_REMOTE_APPLY_FAILED;
        result.disk_reader_mismatch_possible = 1;
    } else {
        result.phase = CN_READER_CONFLICT_REMOTE_APPLIED;
        result.redraw_needed = result.reader_apply.redraw_needed;
    }
    return result;
}

const char *cn_reader_conflict_phase_name(cn_reader_conflict_phase phase)
{
    static const char *const names[] = {
        "invalid", "cancelled", "stale", "local-complete",
        "remote-complete", "remote-applied", "remote-apply-failed"
    };
    return phase >= CN_READER_CONFLICT_INVALID &&
           phase <= CN_READER_CONFLICT_REMOTE_APPLY_FAILED
               ? names[phase] : "unknown";
}
