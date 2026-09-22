/* Caller-rooted, bounded application storage layout. */
#ifndef CN_STORAGE_STORAGE_LAYOUT_H
#define CN_STORAGE_STORAGE_LAYOUT_H

#include <stddef.h>

#define CN_STORAGE_PATH_CAPACITY 4096
#define CN_STORAGE_PATH_MAX_BYTES (CN_STORAGE_PATH_CAPACITY - 1)

typedef enum cn_storage_location {
    CN_STORAGE_LOCATION_CONFIG = 0,
    CN_STORAGE_LOCATION_PROGRESS,
    CN_STORAGE_LOCATION_COUNT
} cn_storage_location;

typedef enum cn_storage_result {
    CN_STORAGE_OK = 0,
    CN_STORAGE_INVALID,
    CN_STORAGE_PATH_TOO_LONG,
    CN_STORAGE_BUFFER_TOO_SMALL,
    CN_STORAGE_NOT_FOUND,
    CN_STORAGE_NOT_DIRECTORY,
    CN_STORAGE_SYMLINK,
    CN_STORAGE_PERMISSION_DENIED,
    CN_STORAGE_READ_ONLY,
    CN_STORAGE_CREATE_FAILED,
    CN_STORAGE_IO_ERROR,
    CN_STORAGE_RESULT_COUNT
} cn_storage_result;

typedef struct cn_storage_layout {
    char root[CN_STORAGE_PATH_CAPACITY];
    size_t root_length;
} cn_storage_layout;

/* Validates and copies an existing absolute root without changing it. */
cn_storage_result cn_storage_layout_init(cn_storage_layout *layout,
                                         const char *root,
                                         int *system_errno);

/* Derives a fixed location without accessing the filesystem. */
cn_storage_result cn_storage_layout_path(const cn_storage_layout *layout,
                                         cn_storage_location location,
                                         char *out, size_t capacity);

/* Idempotently creates state, state/progress, and config below the root. */
cn_storage_result cn_storage_layout_prepare(const cn_storage_layout *layout,
                                            int *system_errno);

const char *cn_storage_result_name(cn_storage_result result);

#endif /* CN_STORAGE_STORAGE_LAYOUT_H */
