/*
 * progress_store.h - bounded local persistence for logical ReaderPosition.
 *
 * The caller supplies an existing writable state directory. Book identities
 * select records; physical page numbers are never stored or used to restore.
 */
#ifndef CN_PROGRESS_PROGRESS_STORE_H
#define CN_PROGRESS_PROGRESS_STORE_H

#include "progress/book_identity.h"
#include "reader/reader.h"

#define CN_PROGRESS_FORMAT_VERSION 1
#define CN_PROGRESS_FILE_SUFFIX ".progress"

typedef struct cn_progress_store cn_progress_store;

typedef struct cn_progress_record {
    cn_reader_position position;
} cn_progress_record;

typedef enum cn_progress_result {
    CN_PROGRESS_OK = 0,
    CN_PROGRESS_MISSING,
    CN_PROGRESS_CORRUPT,
    CN_PROGRESS_UNSUPPORTED,
    CN_PROGRESS_IO_ERROR,
    CN_PROGRESS_INVALID,
    CN_PROGRESS_NO_MEMORY
} cn_progress_result;

void cn_progress_record_init(cn_progress_record *record);
void cn_progress_record_clear(cn_progress_record *record);

/* state_directory must already exist and is copied by the store. */
cn_progress_result cn_progress_store_open(cn_progress_store **out,
                                          const char *state_directory);
void cn_progress_store_close(cn_progress_store *store);

/* load replaces an initialized record only on success. */
cn_progress_result cn_progress_store_load(cn_progress_store *store,
                                          const cn_book_identity *identity,
                                          cn_progress_record *record);
cn_progress_result cn_progress_store_save(cn_progress_store *store,
                                          const cn_book_identity *identity,
                                          const cn_progress_record *record);

const char *cn_progress_result_name(cn_progress_result result);

#endif /* CN_PROGRESS_PROGRESS_STORE_H */
