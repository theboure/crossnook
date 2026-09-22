#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "storage/storage_layout.h"

#define CONFIG_SUFFIX "/config"
#define STATE_SUFFIX "/state"
#define PROGRESS_SUFFIX "/state/progress"

static void set_system_errno(int *system_errno, int value)
{
    if (system_errno)
        *system_errno = value;
}

static cn_storage_result inspection_error(int error)
{
    switch (error) {
    case ENOENT:
        return CN_STORAGE_NOT_FOUND;
    case ENOTDIR:
        return CN_STORAGE_NOT_DIRECTORY;
    case EACCES:
    case EPERM:
        return CN_STORAGE_PERMISSION_DENIED;
    case ENAMETOOLONG:
        return CN_STORAGE_PATH_TOO_LONG;
    default:
        return CN_STORAGE_IO_ERROR;
    }
}

static cn_storage_result creation_error(int error)
{
    switch (error) {
    case ENOTDIR:
        return CN_STORAGE_NOT_DIRECTORY;
    case EACCES:
    case EPERM:
        return CN_STORAGE_PERMISSION_DENIED;
    case EROFS:
        return CN_STORAGE_READ_ONLY;
    case ENAMETOOLONG:
        return CN_STORAGE_PATH_TOO_LONG;
    default:
        return CN_STORAGE_CREATE_FAILED;
    }
}

static cn_storage_result bounded_length(const char *text, size_t *length)
{
    size_t i;
    if (!text)
        return CN_STORAGE_INVALID;
    for (i = 0; i < CN_STORAGE_PATH_CAPACITY; ++i) {
        if (text[i] == '\0') {
            *length = i;
            return CN_STORAGE_OK;
        }
    }
    return CN_STORAGE_PATH_TOO_LONG;
}

static int is_dot_component(const char *root, size_t begin, size_t end)
{
    size_t length = end - begin;
    return (length == 1 && root[begin] == '.') ||
           (length == 2 && root[begin] == '.' && root[begin + 1] == '.');
}

static cn_storage_result validate_normalized_root(const char *root,
                                                  size_t length)
{
    size_t component = 1;
    size_t i;
    if (!root || length <= 1 || root[0] != '/' ||
        root[length - 1] == '/')
        return CN_STORAGE_INVALID;
    for (i = 1; i <= length; ++i) {
        if (i == length || root[i] == '/') {
            if (i == component || is_dot_component(root, component, i))
                return CN_STORAGE_INVALID;
            component = i + 1;
        }
    }
    if (length > CN_STORAGE_PATH_MAX_BYTES -
                     (sizeof PROGRESS_SUFFIX - 1))
        return CN_STORAGE_PATH_TOO_LONG;
    return CN_STORAGE_OK;
}

static int layout_valid(const cn_storage_layout *layout)
{
    size_t length;
    if (!layout || layout->root_length == 0 ||
        layout->root_length >= CN_STORAGE_PATH_CAPACITY ||
        bounded_length(layout->root, &length) != CN_STORAGE_OK ||
        length != layout->root_length)
        return 0;
    return validate_normalized_root(layout->root, length) == CN_STORAGE_OK;
}

static cn_storage_result inspect_directory(const char *path,
                                           int missing_is_ok,
                                           int *system_errno)
{
    struct stat st;
    int error;
    if (lstat(path, &st) != 0) {
        error = errno;
        if (missing_is_ok && error == ENOENT)
            return CN_STORAGE_NOT_FOUND;
        set_system_errno(system_errno, error);
        return inspection_error(error);
    }
    if (S_ISLNK(st.st_mode))
        return CN_STORAGE_SYMLINK;
    if (!S_ISDIR(st.st_mode))
        return CN_STORAGE_NOT_DIRECTORY;
    return CN_STORAGE_OK;
}

static cn_storage_result build_path(const cn_storage_layout *layout,
                                    const char *suffix,
                                    char *out, size_t capacity)
{
    size_t suffix_length = strlen(suffix);
    size_t required;
    if (!layout_valid(layout) || !out)
        return CN_STORAGE_INVALID;
    required = layout->root_length + suffix_length + 1;
    if (capacity < required) {
        if (capacity > 0)
            out[0] = '\0';
        return CN_STORAGE_BUFFER_TOO_SMALL;
    }
    memcpy(out, layout->root, layout->root_length);
    memcpy(out + layout->root_length, suffix, suffix_length + 1);
    return CN_STORAGE_OK;
}

static cn_storage_result ensure_directory(const cn_storage_layout *layout,
                                          const char *suffix,
                                          int *system_errno)
{
    char path[CN_STORAGE_PATH_CAPACITY];
    cn_storage_result result;
    int error;

    result = build_path(layout, suffix, path, sizeof path);
    if (result != CN_STORAGE_OK)
        return result;
    result = inspect_directory(path, 1, system_errno);
    if (result == CN_STORAGE_OK || result != CN_STORAGE_NOT_FOUND)
        return result;

    if (mkdir(path, 0700) == 0)
        return CN_STORAGE_OK;
    error = errno;
    if (error != EEXIST) {
        set_system_errno(system_errno, error);
        return creation_error(error);
    }

    /* A concurrent creator is accepted only after fresh type inspection.
     * This is race handling, not an adversarial filesystem sandbox. */
    set_system_errno(system_errno, 0);
    return inspect_directory(path, 0, system_errno);
}

cn_storage_result cn_storage_layout_init(cn_storage_layout *layout,
                                         const char *root,
                                         int *system_errno)
{
    cn_storage_result result;
    size_t length;

    set_system_errno(system_errno, 0);
    if (!layout)
        return CN_STORAGE_INVALID;
    memset(layout, 0, sizeof *layout);

    result = bounded_length(root, &length);
    if (result != CN_STORAGE_OK)
        return result;
    if (length == 0 || root[0] != '/')
        return CN_STORAGE_INVALID;
    while (length > 1 && root[length - 1] == '/')
        --length;
    result = validate_normalized_root(root, length);
    if (result != CN_STORAGE_OK)
        return result;

    memcpy(layout->root, root, length);
    layout->root[length] = '\0';
    layout->root_length = length;
    result = inspect_directory(layout->root, 0, system_errno);
    if (result != CN_STORAGE_OK)
        memset(layout, 0, sizeof *layout);
    return result;
}

cn_storage_result cn_storage_layout_path(const cn_storage_layout *layout,
                                         cn_storage_location location,
                                         char *out, size_t capacity)
{
    const char *suffix;
    if (out && capacity > 0)
        out[0] = '\0';
    if (!out)
        return CN_STORAGE_INVALID;
    if (location < CN_STORAGE_LOCATION_CONFIG ||
        location >= CN_STORAGE_LOCATION_COUNT)
        return CN_STORAGE_INVALID;
    suffix = location == CN_STORAGE_LOCATION_CONFIG
                 ? CONFIG_SUFFIX : PROGRESS_SUFFIX;
    return build_path(layout, suffix, out, capacity);
}

cn_storage_result cn_storage_layout_prepare(const cn_storage_layout *layout,
                                            int *system_errno)
{
    cn_storage_result result;
    set_system_errno(system_errno, 0);
    if (!layout_valid(layout))
        return CN_STORAGE_INVALID;

    result = ensure_directory(layout, STATE_SUFFIX, system_errno);
    if (result != CN_STORAGE_OK)
        return result;
    result = ensure_directory(layout, PROGRESS_SUFFIX, system_errno);
    if (result != CN_STORAGE_OK)
        return result;
    return ensure_directory(layout, CONFIG_SUFFIX, system_errno);
}

const char *cn_storage_result_name(cn_storage_result result)
{
    static const char *const names[CN_STORAGE_RESULT_COUNT] = {
        "ok", "invalid", "path-too-long", "buffer-too-small",
        "not-found", "not-directory", "symlink", "permission-denied",
        "read-only", "create-failed", "io-error"
    };
    if (result < 0 || result >= CN_STORAGE_RESULT_COUNT)
        return "unknown";
    return names[result];
}
