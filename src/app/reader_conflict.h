/* Application-owned composition of an already established Reader conflict. */
#ifndef CN_APP_READER_CONFLICT_H
#define CN_APP_READER_CONFLICT_H

#include "app/reader_sync.h"

typedef enum cn_reader_conflict_choice {
    CN_READER_CONFLICT_CANCEL = 0,
    CN_READER_CONFLICT_USE_LOCAL,
    CN_READER_CONFLICT_USE_REMOTE
} cn_reader_conflict_choice;

typedef enum cn_reader_conflict_phase {
    CN_READER_CONFLICT_INVALID = 0,
    CN_READER_CONFLICT_CANCELLED,
    CN_READER_CONFLICT_STALE,
    CN_READER_CONFLICT_LOCAL_COMPLETE,
    CN_READER_CONFLICT_REMOTE_COMPLETE,
    CN_READER_CONFLICT_REMOTE_APPLIED,
    CN_READER_CONFLICT_REMOTE_APPLY_FAILED
} cn_reader_conflict_phase;

/* Bounded evidence only. No owned paths, positions, or credentials. */
typedef struct cn_reader_conflict_result {
    cn_reader_conflict_phase phase;
    cn_progress_result local_check_result;
    cn_sync_push_result push;
    cn_sync_pull_result pull;
    cn_reader_sync_result reader_apply;
    int push_invoked;
    int pull_invoked;
    int redraw_needed;
    int disk_reader_mismatch_possible;
} cn_reader_conflict_result;

int cn_reader_sync_is_conflict(const cn_reader_sync_result *original);

/* original is the completed save-first normal sync. The caller owns a deep
 * copy of expected_position and an exact-path copy of expected_book_path for
 * the dialog lifetime. All inputs are borrowed for this synchronous call.
 * Cancel performs no checks, persistence, or network. Other choices reject
 * changed Reader/book/local state before invoking an explicit controller. */
cn_reader_conflict_result cn_reader_sync_resolve_conflict(
    const cn_reader_sync_config *config,
    const cn_reader_sync_result *original,
    const cn_reader_position *expected_position,
    const char *expected_book_path,
    cn_reader_conflict_choice choice);

const char *cn_reader_conflict_phase_name(cn_reader_conflict_phase phase);

#endif /* CN_APP_READER_CONFLICT_H */
