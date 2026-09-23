/* Bounded, location-neutral persistence for KOSync authentication material. */
#ifndef CN_CREDENTIALS_CREDENTIAL_STORE_H
#define CN_CREDENTIALS_CREDENTIAL_STORE_H

#include <stddef.h>

#define CN_CREDENTIAL_USERNAME_MAX 255
#define CN_CREDENTIAL_USERKEY_MAX 255
#define CN_CREDENTIAL_PATH_CAPACITY 4096
#define CN_CREDENTIAL_FILE_MAX_BYTES 1024

typedef struct cn_credentials {
    char username[CN_CREDENTIAL_USERNAME_MAX + 1];
    char userkey[CN_CREDENTIAL_USERKEY_MAX + 1];
} cn_credentials;

typedef struct cn_credential_store {
    char directory[CN_CREDENTIAL_PATH_CAPACITY];
    size_t directory_length;
} cn_credential_store;

typedef enum cn_credential_result {
    CN_CREDENTIAL_OK = 0,
    CN_CREDENTIAL_MISSING,
    CN_CREDENTIAL_INVALID,
    CN_CREDENTIAL_CORRUPT,
    CN_CREDENTIAL_UNSUPPORTED_VERSION,
    CN_CREDENTIAL_PATH_TOO_LONG,
    CN_CREDENTIAL_NOT_FOUND,
    CN_CREDENTIAL_NOT_DIRECTORY,
    CN_CREDENTIAL_SYMLINK,
    CN_CREDENTIAL_NON_REGULAR,
    CN_CREDENTIAL_PERMISSION_DENIED,
    CN_CREDENTIAL_READ_ONLY,
    CN_CREDENTIAL_NO_SPACE,
    CN_CREDENTIAL_IO_ERROR,
    CN_CREDENTIAL_DURABILITY_UNCERTAIN,
    CN_CREDENTIAL_RESULT_COUNT
} cn_credential_result;

void cn_credentials_clear(cn_credentials *credentials);
/* Copies and inspects an existing absolute directory. No mount discovery. */
cn_credential_result cn_credential_store_init(cn_credential_store *store,
                                               const char *directory,
                                               int *system_errno);
/* On every non-OK result, clears all output bytes. Never creates a file. */
cn_credential_result cn_credential_store_load(const cn_credential_store *store,
                                               cn_credentials *credentials,
                                               int *system_errno);
/* A post-rename DURABILITY_UNCERTAIN means the new file is visible now. */
cn_credential_result cn_credential_store_save(const cn_credential_store *store,
                                               const cn_credentials *credentials,
                                               int *system_errno);
const char *cn_credential_result_name(cn_credential_result result);

#endif /* CN_CREDENTIALS_CREDENTIAL_STORE_H */
