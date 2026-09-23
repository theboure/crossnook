/* Bounded persistence for application settings. */
#ifndef CN_SETTINGS_SETTINGS_STORE_H
#define CN_SETTINGS_SETTINGS_STORE_H

#include <stddef.h>

#define CN_SETTINGS_FORMAT_VERSION 1
#define CN_SETTINGS_PATH_CAPACITY 4096
#define CN_SETTINGS_KOSYNC_URL_MAX 1296
#define CN_SETTINGS_KOSYNC_URL_CAPACITY (CN_SETTINGS_KOSYNC_URL_MAX + 1)
#define CN_SETTINGS_DEVICE_NAME_MAX 255
#define CN_SETTINGS_DEVICE_NAME_CAPACITY (CN_SETTINGS_DEVICE_NAME_MAX + 1)
#define CN_SETTINGS_FILE_MAX_BYTES 2048

typedef struct cn_settings {
    int kosync_enabled;
    char kosync_base_url[CN_SETTINGS_KOSYNC_URL_CAPACITY];
    char kosync_device_name[CN_SETTINGS_DEVICE_NAME_CAPACITY];
} cn_settings;

typedef struct cn_settings_store {
    char config_directory[CN_SETTINGS_PATH_CAPACITY];
    size_t config_directory_length;
} cn_settings_store;

typedef enum cn_settings_result {
    CN_SETTINGS_OK = 0,
    CN_SETTINGS_MISSING,
    CN_SETTINGS_INVALID_ARGUMENT,
    CN_SETTINGS_INVALID_SETTINGS,
    CN_SETTINGS_CORRUPT,
    CN_SETTINGS_UNSUPPORTED_VERSION,
    CN_SETTINGS_PATH_TOO_LONG,
    CN_SETTINGS_NOT_FOUND,
    CN_SETTINGS_NOT_DIRECTORY,
    CN_SETTINGS_SYMLINK,
    CN_SETTINGS_NON_REGULAR,
    CN_SETTINGS_PERMISSION_DENIED,
    CN_SETTINGS_READ_ONLY,
    CN_SETTINGS_NO_SPACE,
    CN_SETTINGS_IO_ERROR,
    CN_SETTINGS_DURABILITY_UNCERTAIN,
    CN_SETTINGS_RESULT_COUNT
} cn_settings_result;

void cn_settings_defaults(cn_settings *settings);
cn_settings_result cn_settings_validate(const cn_settings *settings);

/* config_directory must already exist and is copied by the store. */
cn_settings_result cn_settings_store_init(cn_settings_store *store,
                                          const char *config_directory,
                                          int *system_errno);

/* A valid output receives defaults for every result other than OK. */
cn_settings_result cn_settings_load(const cn_settings_store *store,
                                    cn_settings *settings,
                                    int *system_errno);

cn_settings_result cn_settings_save(const cn_settings_store *store,
                                    const cn_settings *settings,
                                    int *system_errno);

const char *cn_settings_result_name(cn_settings_result result);

#endif /* CN_SETTINGS_SETTINGS_STORE_H */
