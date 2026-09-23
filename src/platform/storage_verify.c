#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "platform/storage_verify.h"

static void record_errno(int *out, int error)
{
    if (out)
        *out = error;
}

static cn_platform_storage_result normalize(const char *input, char *out)
{
    size_t i, length;
    if (!input || input[0] != '/')
        return CN_PLATFORM_STORAGE_INVALID;
    for (length = 0; length < CN_PLATFORM_STORAGE_PATH_CAPACITY; ++length)
        if (!input[length])
            break;
    if (length == CN_PLATFORM_STORAGE_PATH_CAPACITY)
        return CN_PLATFORM_STORAGE_PATH_TOO_LONG;
    while (length > 1 && input[length - 1] == '/')
        --length;
    if (length <= 1)
        return CN_PLATFORM_STORAGE_INVALID;
    for (i = 1; i < length; ++i) {
        size_t start = i;
        while (i < length && input[i] != '/')
            ++i;
        if (i == start || (i - start == 1 && input[start] == '.') ||
            (i - start == 2 && input[start] == '.' && input[start + 1] == '.'))
            return CN_PLATFORM_STORAGE_INVALID;
    }
    memcpy(out, input, length);
    out[length] = 0;
    return CN_PLATFORM_STORAGE_OK;
}

static int beneath(const char *path, const char *mountpoint)
{
    size_t length = strlen(mountpoint);
    if (length == 1 && mountpoint[0] == '/')
        return path[0] == '/';
    return strncmp(path, mountpoint, length) == 0 &&
           (path[length] == 0 || path[length] == '/');
}

static int read_live(void *unused, char *out, size_t capacity, size_t *length)
{
    int fd, error;
    size_t done = 0;
    char extra;
    ssize_t got = 0;
    (void)unused;
    fd = open("/proc/self/mountinfo", O_RDONLY);
    if (fd < 0)
        return -1;
    while (done < capacity) {
        got = read(fd, out + done, capacity - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        done += (size_t)got;
    }
    if (got == 0 && done > 0) {
        if (close(fd) == 0) {
            *length = done;
            return 0;
        }
        return -1;
    }
    if (done == capacity && got >= 0) {
        do {
            got = read(fd, &extra, 1);
        } while (got < 0 && errno == EINTR);
        if (got == 0 && close(fd) == 0) {
            *length = done;
            return 0;
        }
        if (got > 0)
            errno = EOVERFLOW;
    }
    error = got == 0 ? EINVAL : errno;
    (void)close(fd);
    errno = error;
    return -1;
}

static int live_lstat(void *unused, const char *path, struct stat *st)
{
    (void)unused;
    return lstat(path, st);
}

static int live_stat(void *unused, const char *path, struct stat *st)
{
    (void)unused;
    return stat(path, st);
}

static int unescape(char *token)
{
    char *read = token, *write = token;
    while (*read) {
        if (*read == '\\') {
            int a, b, c;
            if (strlen(read) < 4 ||
                read[1] < '0' || read[1] > '7' ||
                read[2] < '0' || read[2] > '7' ||
                read[3] < '0' || read[3] > '7')
                return 0;
            a = read[1] - '0'; b = read[2] - '0'; c = read[3] - '0';
            if (a * 64 + b * 8 + c == 0)
                return 0;
            *write++ = (char)(a * 64 + b * 8 + c);
            read += 4;
        } else {
            *write++ = *read++;
        }
    }
    *write = 0;
    return 1;
}

static char *token(char **cursor)
{
    char *start = *cursor;
    char *end;
    if (!start || !*start || *start == ' ')
        return NULL;
    end = strchr(start, ' ');
    if (end) {
        *end = 0;
        *cursor = end + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

static int number(const char *text, unsigned *value)
{
    unsigned result = 0;
    if (!text || !*text)
        return 0;
    while (*text) {
        unsigned digit;
        if (*text < '0' || *text > '9')
            return 0;
        digit = (unsigned)(*text++ - '0');
        if (result > (UINT_MAX - digit) / 10u)
            return 0;
        result = result * 10u + digit;
    }
    *value = result;
    return 1;
}

static int device_number(char *text, unsigned *maj, unsigned *min)
{
    char *colon = strchr(text, ':');
    if (!colon)
        return 0;
    *colon = 0;
    return number(text, maj) && number(colon + 1, min);
}

static int option_present(const char *options, const char *name)
{
    size_t length = strlen(name);
    const char *p = options;
    while (p && *p) {
        const char *next = strchr(p, ',');
        size_t current = next ? (size_t)(next - p) : strlen(p);
        if (current == length && memcmp(p, name, length) == 0)
            return 1;
        p = next ? next + 1 : NULL;
    }
    return 0;
}

typedef struct mount_entry {
    char *root;
    char *point;
    char *options;
    char *type;
    char *device;
    char *super_options;
    unsigned major_number;
    unsigned minor_number;
} mount_entry;

static int parse_mount(char *line, mount_entry *entry)
{
    char *cursor = line;
    char *id = token(&cursor);
    char *parent = token(&cursor);
    char *numbers = token(&cursor);
    char *optional;
    unsigned ignored;
    if (!id || !parent || !numbers || !number(id, &ignored) ||
        !number(parent, &ignored) ||
        !device_number(numbers, &entry->major_number, &entry->minor_number))
        return 0;
    entry->root = token(&cursor);
    entry->point = token(&cursor);
    entry->options = token(&cursor);
    if (!entry->root || !entry->point || !entry->options || !cursor ||
        !unescape(entry->root) || !unescape(entry->point) ||
        entry->root[0] != '/' || entry->point[0] != '/')
        return 0;
    do {
        optional = token(&cursor);
        if (!optional)
            return 0;
    } while (strcmp(optional, "-") != 0);
    entry->type = token(&cursor);
    entry->device = token(&cursor);
    entry->super_options = token(&cursor);
    return entry->type && entry->device && entry->super_options &&
           cursor == NULL && unescape(entry->device);
}

static cn_platform_storage_result inspect_components(
    const cn_platform_storage_source *source, const char *path,
    dev_t expected, int missing_result, int *system_errno)
{
    char component[CN_PLATFORM_STORAGE_PATH_CAPACITY];
    size_t i, length = strlen(path);
    struct stat st;
    for (i = 1; i <= length; ++i) {
        int error;
        if (i != length && path[i] != '/')
            continue;
        memcpy(component, path, i);
        component[i] = 0;
        if (source->lstat_path(source->context, component, &st) != 0) {
            error = errno;
            record_errno(system_errno, error);
            return error == ENOENT ? (cn_platform_storage_result)missing_result
                                   : CN_PLATFORM_STORAGE_IO_ERROR;
        }
        if (S_ISLNK(st.st_mode))
            return CN_PLATFORM_STORAGE_SYMLINK;
        if (!S_ISDIR(st.st_mode))
            return CN_PLATFORM_STORAGE_NOT_DIRECTORY;
        if (i == length && st.st_dev != expected)
            return CN_PLATFORM_STORAGE_WRONG_MOUNT;
    }
    return CN_PLATFORM_STORAGE_OK;
}

cn_platform_storage_result cn_platform_storage_verify_with_source(
    const cn_platform_storage_candidate *candidate,
    const cn_platform_storage_source *source,
    cn_platform_storage_verified *verified, int *system_errno)
{
    char root[CN_PLATFORM_STORAGE_PATH_CAPACITY];
    char point[CN_PLATFORM_STORAGE_PATH_CAPACITY];
    char mounts[CN_PLATFORM_MOUNTINFO_CAPACITY + 1];
    char selected_device[CN_PLATFORM_STORAGE_PATH_CAPACITY];
    cn_platform_storage_result result;
    size_t length, offset, longest = 0;
    unsigned selected_major = 0, selected_minor = 0;
    int selected = 0, duplicates = 0, rw = 0, unsupported = 0;
    struct stat st;
    record_errno(system_errno, 0);
    if (verified)
        memset(verified, 0, sizeof *verified);
    if (!candidate || !source || !verified || !source->read_mountinfo ||
        !source->lstat_path || !source->stat_path ||
        (!candidate->expected_major && !candidate->expected_minor))
        return CN_PLATFORM_STORAGE_INVALID;
    result = normalize(candidate->root, root);
    if (result != CN_PLATFORM_STORAGE_OK)
        return result;
    result = normalize(candidate->mountpoint, point);
    if (result != CN_PLATFORM_STORAGE_OK)
        return result;
    if (!beneath(root, point))
        return CN_PLATFORM_STORAGE_WRONG_MOUNT;
    if (source->read_mountinfo(source->context, mounts,
                               CN_PLATFORM_MOUNTINFO_CAPACITY, &length) != 0) {
        record_errno(system_errno, errno);
        return CN_PLATFORM_STORAGE_IO_ERROR;
    }
    if (length == 0 || length > CN_PLATFORM_MOUNTINFO_CAPACITY ||
        mounts[length - 1] != '\n' || memchr(mounts, 0, length))
        return CN_PLATFORM_STORAGE_MALFORMED_MOUNTS;
    mounts[length] = 0;
    selected_device[0] = 0;
    for (offset = 0; offset < length;) {
        char *line = mounts + offset;
        char *end = strchr(line, '\n');
        mount_entry entry;
        size_t current;
        if (!end || end - line > 8192)
            return CN_PLATFORM_STORAGE_MALFORMED_MOUNTS;
        offset = (size_t)(end - mounts) + 1;
        *end = 0;
        if (!parse_mount(line, &entry))
            return CN_PLATFORM_STORAGE_MALFORMED_MOUNTS;
        if (!beneath(root, entry.point))
            continue;
        current = strlen(entry.point);
        if (current > longest) {
            size_t source_length = strlen(entry.device);
            if (source_length >= sizeof selected_device)
                return CN_PLATFORM_STORAGE_MALFORMED_MOUNTS;
            longest = current;
            selected = strcmp(entry.point, point) == 0;
            duplicates = 0;
            selected_major = entry.major_number;
            selected_minor = entry.minor_number;
            memcpy(selected_device, entry.device, source_length + 1);
            rw = option_present(entry.options, "rw") &&
                 option_present(entry.super_options, "rw") &&
                 !option_present(entry.options, "ro") &&
                 !option_present(entry.super_options, "ro");
            unsupported = strcmp(entry.root, "/") != 0 ||
                          (strcmp(entry.type, "vfat") != 0 &&
                           strcmp(entry.type, "ext2") != 0 &&
                           strcmp(entry.type, "ext3") != 0 &&
                           strcmp(entry.type, "ext4") != 0);
        } else if (current == longest) {
            ++duplicates;
        }
    }
    if (!longest)
        return CN_PLATFORM_STORAGE_MOUNT_MISSING;
    if (duplicates)
        return CN_PLATFORM_STORAGE_MALFORMED_MOUNTS;
    if (!selected)
        return longest == 1 ? CN_PLATFORM_STORAGE_MOUNT_MISSING
                            : CN_PLATFORM_STORAGE_WRONG_MOUNT;
    if (unsupported || selected_device[0] != '/')
        return CN_PLATFORM_STORAGE_UNSUPPORTED;
    if (!rw)
        return CN_PLATFORM_STORAGE_READ_ONLY;
    if (selected_major != candidate->expected_major ||
        selected_minor != candidate->expected_minor)
        return CN_PLATFORM_STORAGE_WRONG_DEVICE;
    if (major(makedev(selected_major, selected_minor)) != selected_major ||
        minor(makedev(selected_major, selected_minor)) != selected_minor)
        return CN_PLATFORM_STORAGE_MALFORMED_MOUNTS;
    if (source->stat_path(source->context, selected_device, &st) != 0) {
        record_errno(system_errno, errno);
        return CN_PLATFORM_STORAGE_IO_ERROR;
    }
    if (!S_ISBLK(st.st_mode) || major(st.st_rdev) != selected_major ||
        minor(st.st_rdev) != selected_minor)
        return CN_PLATFORM_STORAGE_WRONG_DEVICE;
    result = inspect_components(source, point, makedev(selected_major,
                                                        selected_minor),
                                CN_PLATFORM_STORAGE_MOUNT_MISSING,
                                system_errno);
    if (result != CN_PLATFORM_STORAGE_OK)
        return result;
    result = inspect_components(source, root, makedev(selected_major,
                                                       selected_minor),
                                CN_PLATFORM_STORAGE_ROOT_MISSING,
                                system_errno);
    if (result != CN_PLATFORM_STORAGE_OK)
        return result;
    memcpy(verified->root, root, strlen(root) + 1);
    memcpy(verified->mountpoint, point, strlen(point) + 1);
    verified->device_major = selected_major;
    verified->device_minor = selected_minor;
    return CN_PLATFORM_STORAGE_OK;
}

cn_platform_storage_result cn_platform_storage_verify(
    const cn_platform_storage_candidate *candidate,
    cn_platform_storage_verified *verified, int *system_errno)
{
    const cn_platform_storage_source live = {
        NULL, read_live, live_lstat, live_stat
    };
    return cn_platform_storage_verify_with_source(candidate, &live,
                                                   verified, system_errno);
}

const char *cn_platform_storage_result_name(cn_platform_storage_result result)
{
    static const char *const names[CN_PLATFORM_STORAGE_RESULT_COUNT] = {
        "ok", "invalid", "path-too-long", "mount-missing", "wrong-mount",
        "wrong-device", "read-only", "root-missing", "not-directory",
        "symlink", "unsupported", "malformed-mounts", "io-error"
    };
    if (result < 0 || result >= CN_PLATFORM_STORAGE_RESULT_COUNT)
        return "unknown";
    return names[result];
}
