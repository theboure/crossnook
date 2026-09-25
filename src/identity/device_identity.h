/* Persistent installation-scoped CrossNook device identity. */
#ifndef CN_IDENTITY_DEVICE_IDENTITY_H
#define CN_IDENTITY_DEVICE_IDENTITY_H

#include <stddef.h>

#include "storage/storage_layout.h"

#define CN_DEVICE_ID_BYTES 16
#define CN_DEVICE_ID_TEXT_LENGTH 32
#define CN_DEVICE_ID_TEXT_CAPACITY (CN_DEVICE_ID_TEXT_LENGTH + 1)
#define CN_DEVICE_ID_FILE_MAX_BYTES 128

/* A callback returns zero only after filling the complete requested buffer. */
typedef int (*cn_device_identity_entropy_fn)(
    void *context, unsigned char *output, size_t length);

typedef struct cn_device_identity_store {
    char path[CN_STORAGE_PATH_CAPACITY];
    size_t path_length;
    cn_device_identity_entropy_fn entropy;
    void *entropy_context;
} cn_device_identity_store;

typedef enum cn_device_identity_result {
    CN_DEVICE_ID_OK = 0,
    CN_DEVICE_ID_CREATED,
    CN_DEVICE_ID_INVALID_ARGUMENT,
    CN_DEVICE_ID_BUFFER_TOO_SMALL,
    CN_DEVICE_ID_NOT_FOUND,
    CN_DEVICE_ID_MALFORMED,
    CN_DEVICE_ID_UNSUPPORTED_VERSION,
    CN_DEVICE_ID_BAD_CHECKSUM,
    CN_DEVICE_ID_ENTROPY_FAILED,
    CN_DEVICE_ID_PATH_TOO_LONG,
    CN_DEVICE_ID_NOT_DIRECTORY,
    CN_DEVICE_ID_SYMLINK,
    CN_DEVICE_ID_NON_REGULAR,
    CN_DEVICE_ID_PERMISSION_DENIED,
    CN_DEVICE_ID_READ_ONLY,
    CN_DEVICE_ID_NO_SPACE,
    CN_DEVICE_ID_IO_ERROR,
    CN_DEVICE_ID_DURABILITY_UNCERTAIN,
    CN_DEVICE_ID_RESULT_COUNT
} cn_device_identity_result;

/* The layout must already have been initialized and prepared by the caller. */
cn_device_identity_result cn_device_identity_store_init(
    cn_device_identity_store *store,
    const cn_storage_layout *layout,
    cn_device_identity_entropy_fn entropy,
    void *entropy_context);

/* Loads the existing identity or creates it only when the file is absent. */
cn_device_identity_result cn_device_identity_load_or_create(
    const cn_device_identity_store *store,
    char *output,
    size_t output_capacity,
    int *system_errno);

/* Loads an existing identity without entropy, creation, or rewrite. */
cn_device_identity_result cn_device_identity_load(
    const cn_device_identity_store *store,
    char *output,
    size_t output_capacity,
    int *system_errno);

const char *cn_device_identity_result_name(cn_device_identity_result result);

#endif /* CN_IDENTITY_DEVICE_IDENTITY_H */
