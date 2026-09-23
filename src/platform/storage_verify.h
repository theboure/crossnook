/* Verify an explicitly selected, already-mounted portable storage root. */
#ifndef CN_PLATFORM_STORAGE_VERIFY_H
#define CN_PLATFORM_STORAGE_VERIFY_H

#include <stddef.h>
#include <sys/stat.h>

#define CN_PLATFORM_STORAGE_PATH_CAPACITY 4096
#define CN_PLATFORM_MOUNTINFO_CAPACITY 32768

typedef struct cn_platform_storage_candidate {
    const char *root;
    const char *mountpoint;
    unsigned expected_major;
    unsigned expected_minor;
} cn_platform_storage_candidate;

typedef struct cn_platform_storage_verified {
    char root[CN_PLATFORM_STORAGE_PATH_CAPACITY];
    char mountpoint[CN_PLATFORM_STORAGE_PATH_CAPACITY];
    unsigned device_major;
    unsigned device_minor;
} cn_platform_storage_verified;

typedef enum cn_platform_storage_result {
    CN_PLATFORM_STORAGE_OK = 0,
    CN_PLATFORM_STORAGE_INVALID,
    CN_PLATFORM_STORAGE_PATH_TOO_LONG,
    CN_PLATFORM_STORAGE_MOUNT_MISSING,
    CN_PLATFORM_STORAGE_WRONG_MOUNT,
    CN_PLATFORM_STORAGE_WRONG_DEVICE,
    CN_PLATFORM_STORAGE_READ_ONLY,
    CN_PLATFORM_STORAGE_ROOT_MISSING,
    CN_PLATFORM_STORAGE_NOT_DIRECTORY,
    CN_PLATFORM_STORAGE_SYMLINK,
    CN_PLATFORM_STORAGE_UNSUPPORTED,
    CN_PLATFORM_STORAGE_MALFORMED_MOUNTS,
    CN_PLATFORM_STORAGE_IO_ERROR,
    CN_PLATFORM_STORAGE_RESULT_COUNT
} cn_platform_storage_result;

/* Diagnostic-only source injection: real startup calls verify(), never this
 * entry point. Callbacks must return genuine lstat/stat semantics; read_mountinfo
 * returns a complete bounded snapshot without an appended NUL. */
typedef struct cn_platform_storage_source {
    void *context;
    int (*read_mountinfo)(void *context, char *out, size_t capacity,
                          size_t *length);
    int (*lstat_path)(void *context, const char *path, struct stat *st);
    int (*stat_path)(void *context, const char *path, struct stat *st);
} cn_platform_storage_source;

cn_platform_storage_result cn_platform_storage_verify(
    const cn_platform_storage_candidate *candidate,
    cn_platform_storage_verified *verified, int *system_errno);

cn_platform_storage_result cn_platform_storage_verify_with_source(
    const cn_platform_storage_candidate *candidate,
    const cn_platform_storage_source *source,
    cn_platform_storage_verified *verified, int *system_errno);

const char *cn_platform_storage_result_name(cn_platform_storage_result result);

#endif /* CN_PLATFORM_STORAGE_VERIFY_H */
