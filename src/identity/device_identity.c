#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "identity/device_identity.h"

#define DEVICE_ID_FILE_SUFFIX "/device-id"
#define DEVICE_ID_VERSION_LINE "crossnook-device-identity=1\n"
#define DEVICE_ID_ID_PREFIX "id="
#define DEVICE_ID_CRC_PREFIX "crc32="

static void set_errno_out(int *out, int value)
{
    if (out)
        *out = value;
}

static int bounded_length(const char *text, size_t capacity, size_t *length)
{
    size_t i;
    if (!text || !length)
        return 0;
    for (i = 0; i < capacity; ++i) {
        if (text[i] == '\0') {
            *length = i;
            return 1;
        }
    }
    return 0;
}

static cn_device_identity_result filesystem_error(int error)
{
    switch (error) {
    case ENOENT: return CN_DEVICE_ID_NOT_FOUND;
    case ENOTDIR: return CN_DEVICE_ID_NOT_DIRECTORY;
    case ELOOP: return CN_DEVICE_ID_SYMLINK;
    case EACCES:
    case EPERM: return CN_DEVICE_ID_PERMISSION_DENIED;
    case EROFS: return CN_DEVICE_ID_READ_ONLY;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return CN_DEVICE_ID_NO_SPACE;
    case ENAMETOOLONG: return CN_DEVICE_ID_PATH_TOO_LONG;
    default: return CN_DEVICE_ID_IO_ERROR;
    }
}

static int store_valid(const cn_device_identity_store *store)
{
    size_t length;
    return store && store->path_length > 1 &&
           store->path_length < CN_STORAGE_PATH_CAPACITY &&
           bounded_length(store->path, sizeof store->path, &length) &&
           length == store->path_length && store->path[0] == '/' &&
           store->path[length - 1] != '/';
}

static cn_device_identity_result inspect_config(const char *path,
                                                int *system_errno)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        set_errno_out(system_errno, errno);
        return filesystem_error(errno);
    }
    if (S_ISLNK(st.st_mode))
        return CN_DEVICE_ID_SYMLINK;
    return S_ISDIR(st.st_mode) ? CN_DEVICE_ID_OK : CN_DEVICE_ID_NOT_DIRECTORY;
}

static cn_device_identity_result inspect_identity_file(
    const char *path, struct stat *st, int *system_errno)
{
    if (lstat(path, st) != 0) {
        if (errno == ENOENT)
            return CN_DEVICE_ID_NOT_FOUND;
        set_errno_out(system_errno, errno);
        return filesystem_error(errno);
    }
    if (S_ISLNK(st->st_mode))
        return CN_DEVICE_ID_SYMLINK;
    return S_ISREG(st->st_mode) ? CN_DEVICE_ID_OK : CN_DEVICE_ID_NON_REGULAR;
}

static uint32_t crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    int bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^
                  (UINT32_C(0xedb88320) & (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
}

static int line(const unsigned char *data, size_t size, size_t *offset,
                const unsigned char **out, size_t *length)
{
    size_t begin = *offset;
    size_t i = begin;
    if (begin >= size)
        return 0;
    while (i < size && data[i] != '\n')
        ++i;
    if (i == size)
        return 0;
    *out = data + begin;
    *length = i - begin;
    *offset = i + 1;
    return 1;
}

static cn_device_identity_result parse_identity(const unsigned char *data,
                                                size_t size, char output[CN_DEVICE_ID_TEXT_CAPACITY])
{
    const unsigned char *lines[3];
    size_t lengths[3], offset = 0, i;
    uint32_t expected = 0, actual;

    if (!data || !output || size == 0 || size > CN_DEVICE_ID_FILE_MAX_BYTES ||
        memchr(data, 0, size))
        return CN_DEVICE_ID_MALFORMED;
    for (i = 0; i < 3; ++i)
        if (!line(data, size, &offset, &lines[i], &lengths[i]))
            return CN_DEVICE_ID_MALFORMED;
    if (offset != size || lengths[0] < sizeof "crossnook-device-identity=" - 1 ||
        memcmp(lines[0], "crossnook-device-identity=",
               sizeof "crossnook-device-identity=" - 1) != 0)
        return CN_DEVICE_ID_MALFORMED;
    if (lengths[0] != sizeof DEVICE_ID_VERSION_LINE - 2 ||
        memcmp(lines[0], DEVICE_ID_VERSION_LINE,
               sizeof DEVICE_ID_VERSION_LINE - 2) != 0)
        return CN_DEVICE_ID_UNSUPPORTED_VERSION;
    if (lengths[1] != sizeof DEVICE_ID_ID_PREFIX - 1 + CN_DEVICE_ID_TEXT_LENGTH ||
        memcmp(lines[1], DEVICE_ID_ID_PREFIX, sizeof DEVICE_ID_ID_PREFIX - 1) != 0)
        return CN_DEVICE_ID_MALFORMED;
    for (i = 0; i < CN_DEVICE_ID_TEXT_LENGTH; ++i) {
        unsigned char c = lines[1][sizeof DEVICE_ID_ID_PREFIX - 1 + i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return CN_DEVICE_ID_MALFORMED;
        output[i] = (char)c;
    }
    output[CN_DEVICE_ID_TEXT_LENGTH] = '\0';
    if (lengths[2] != sizeof DEVICE_ID_CRC_PREFIX - 1 + 8 ||
        memcmp(lines[2], DEVICE_ID_CRC_PREFIX, sizeof DEVICE_ID_CRC_PREFIX - 1) != 0)
        return CN_DEVICE_ID_MALFORMED;
    for (i = sizeof DEVICE_ID_CRC_PREFIX - 1; i < lengths[2]; ++i) {
        unsigned digit;
        unsigned char c = lines[2][i];
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10u;
        else return CN_DEVICE_ID_MALFORMED;
        expected = (expected << 4) | digit;
    }
    actual = crc32(data, (size_t)(lines[2] - data));
    return expected == actual ? CN_DEVICE_ID_OK : CN_DEVICE_ID_BAD_CHECKSUM;
}

static cn_device_identity_result read_existing(
    const cn_device_identity_store *store, char output[CN_DEVICE_ID_TEXT_CAPACITY],
    int *system_errno)
{
    unsigned char data[CN_DEVICE_ID_FILE_MAX_BYTES];
    struct stat before, opened;
    size_t done = 0;
    ssize_t got;
    unsigned char extra;
    int fd = -1, error;
    cn_device_identity_result result;

    result = inspect_identity_file(store->path, &before, system_errno);
    if (result != CN_DEVICE_ID_OK)
        return result;
    if (before.st_size <= 0 || before.st_size > CN_DEVICE_ID_FILE_MAX_BYTES)
        return CN_DEVICE_ID_MALFORMED;
    fd = open(store->path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        set_errno_out(system_errno, errno);
        return filesystem_error(errno);
    }
    if (fstat(fd, &opened) != 0) {
        error = errno; set_errno_out(system_errno, error);
        result = filesystem_error(error); goto done;
    }
    if (!S_ISREG(opened.st_mode) || opened.st_dev != before.st_dev ||
        opened.st_ino != before.st_ino) {
        result = CN_DEVICE_ID_IO_ERROR; goto done;
    }
    if (opened.st_size <= 0 || opened.st_size > CN_DEVICE_ID_FILE_MAX_BYTES) {
        result = CN_DEVICE_ID_MALFORMED; goto done;
    }
    while (done < (size_t)opened.st_size) {
        got = read(fd, data + done, (size_t)opened.st_size - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            if (got < 0) {
                set_errno_out(system_errno, errno);
                result = filesystem_error(errno);
            } else result = CN_DEVICE_ID_MALFORMED;
            goto done;
        }
        done += (size_t)got;
    }
    do { got = read(fd, &extra, 1); } while (got < 0 && errno == EINTR);
    if (got != 0) {
        if (got < 0) {
            set_errno_out(system_errno, errno);
            result = filesystem_error(errno);
        } else result = CN_DEVICE_ID_MALFORMED;
        goto done;
    }
    result = parse_identity(data, done, output);
done:
    if (close(fd) != 0 && result == CN_DEVICE_ID_OK) {
        set_errno_out(system_errno, errno);
        result = CN_DEVICE_ID_IO_ERROR;
    }
    if (result != CN_DEVICE_ID_OK)
        output[0] = '\0';
    return result;
}

static int production_entropy(void *context, unsigned char *output, size_t length)
{
    size_t done = 0;
    ssize_t got;
    int fd;
    (void)context;
    if (!output || length != CN_DEVICE_ID_BYTES)
        return -1;
    fd = open("/dev/urandom", O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;
    while (done < length) {
        got = read(fd, output + done, length - done);
        if (got > 0) { done += (size_t)got; continue; }
        if (got < 0 && errno == EINTR) continue;
        (void)close(fd);
        return -1;
    }
    if (close(fd) != 0)
        return -1;
    return 0;
}

static size_t serialize(const unsigned char random_bytes[CN_DEVICE_ID_BYTES],
                        char output[CN_DEVICE_ID_TEXT_CAPACITY],
                        unsigned char file[CN_DEVICE_ID_FILE_MAX_BYTES])
{
    static const char hex[] = "0123456789abcdef";
    size_t i, length;
    int written;
    for (i = 0; i < CN_DEVICE_ID_BYTES; ++i) {
        output[i * 2] = hex[random_bytes[i] >> 4];
        output[i * 2 + 1] = hex[random_bytes[i] & 15];
    }
    output[CN_DEVICE_ID_TEXT_LENGTH] = '\0';
    written = snprintf((char *)file, CN_DEVICE_ID_FILE_MAX_BYTES,
                       "%s%s%s\n", DEVICE_ID_VERSION_LINE,
                       DEVICE_ID_ID_PREFIX, output);
    if (written < 0 || (size_t)written + sizeof DEVICE_ID_CRC_PREFIX + 8 >=
                            CN_DEVICE_ID_FILE_MAX_BYTES)
        return 0;
    length = (size_t)written;
    written = snprintf((char *)file + length,
                       CN_DEVICE_ID_FILE_MAX_BYTES - length, "%s%08x\n",
                       DEVICE_ID_CRC_PREFIX, crc32(file, length));
    if (written < 0)
        return 0;
    return strlen((char *)file);
}

static cn_device_identity_result write_new(const cn_device_identity_store *store,
                                           const unsigned char *data, size_t size,
                                           int *system_errno)
{
    size_t done = 0;
    ssize_t written;
    int fd, error;
    fd = open(store->path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        set_errno_out(system_errno, errno);
        return errno == EEXIST ? CN_DEVICE_ID_IO_ERROR : filesystem_error(errno);
    }
    while (done < size) {
        written = write(fd, data + done, size - done);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            error = written < 0 ? errno : EIO;
            set_errno_out(system_errno, error);
            (void)close(fd); (void)unlink(store->path);
            return filesystem_error(error);
        }
        done += (size_t)written;
    }
    if (fsync(fd) != 0) {
        set_errno_out(system_errno, errno);
        (void)close(fd);
        return CN_DEVICE_ID_DURABILITY_UNCERTAIN;
    }
    if (close(fd) != 0) {
        set_errno_out(system_errno, errno);
        return CN_DEVICE_ID_DURABILITY_UNCERTAIN;
    }
    {
        char directory[CN_STORAGE_PATH_CAPACITY];
        char *slash = strrchr(store->path, '/');
        int directory_fd;
        struct stat before, after;
        size_t length;
        if (!slash)
            return CN_DEVICE_ID_DURABILITY_UNCERTAIN;
        length = (size_t)(slash - store->path);
        if (length == 0 || length >= sizeof directory)
            return CN_DEVICE_ID_DURABILITY_UNCERTAIN;
        memcpy(directory, store->path, length);
        directory[length] = '\0';
        if (lstat(directory, &before) != 0) {
            set_errno_out(system_errno, errno);
            return CN_DEVICE_ID_DURABILITY_UNCERTAIN;
        }
        directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        if (directory_fd < 0 || fstat(directory_fd, &after) != 0 ||
            !S_ISDIR(after.st_mode) || before.st_dev != after.st_dev ||
            before.st_ino != after.st_ino || fsync(directory_fd) != 0) {
            set_errno_out(system_errno, errno);
            if (directory_fd >= 0) (void)close(directory_fd);
            return CN_DEVICE_ID_DURABILITY_UNCERTAIN;
        }
        if (close(directory_fd) != 0) {
            set_errno_out(system_errno, errno);
            return CN_DEVICE_ID_DURABILITY_UNCERTAIN;
        }
    }
    return CN_DEVICE_ID_CREATED;
}

cn_device_identity_result cn_device_identity_store_init(
    cn_device_identity_store *store, const cn_storage_layout *layout,
    cn_device_identity_entropy_fn entropy, void *entropy_context)
{
    char config[CN_STORAGE_PATH_CAPACITY];
    size_t length;
    cn_storage_result storage_result;

    if (!store || !layout)
        return CN_DEVICE_ID_INVALID_ARGUMENT;
    memset(store, 0, sizeof *store);
    storage_result = cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG,
                                            config, sizeof config);
    if (storage_result != CN_STORAGE_OK)
        return storage_result == CN_STORAGE_BUFFER_TOO_SMALL
                   ? CN_DEVICE_ID_PATH_TOO_LONG : CN_DEVICE_ID_INVALID_ARGUMENT;
    {
        cn_device_identity_result result = inspect_config(config, NULL);
        if (result != CN_DEVICE_ID_OK)
            return result;
    }
    if (!bounded_length(config, sizeof config, &length) ||
        length + sizeof DEVICE_ID_FILE_SUFFIX > sizeof store->path)
        return CN_DEVICE_ID_PATH_TOO_LONG;
    memcpy(store->path, config, length);
    memcpy(store->path + length, DEVICE_ID_FILE_SUFFIX,
           sizeof DEVICE_ID_FILE_SUFFIX);
    store->path_length = length + sizeof DEVICE_ID_FILE_SUFFIX - 1;
    store->entropy = entropy;
    store->entropy_context = entropy_context;
    return CN_DEVICE_ID_OK;
}

cn_device_identity_result cn_device_identity_load_or_create(
    const cn_device_identity_store *store, char *output, size_t output_capacity,
    int *system_errno)
{
    unsigned char random_bytes[CN_DEVICE_ID_BYTES];
    unsigned char file[CN_DEVICE_ID_FILE_MAX_BYTES];
    char id[CN_DEVICE_ID_TEXT_CAPACITY];
    cn_device_identity_result result;
    size_t size;
    int entropy_result;
    int local_errno = 0;
    int *error_out = system_errno ? system_errno : &local_errno;

    set_errno_out(error_out, 0);
    if (output && output_capacity > 0)
        output[0] = '\0';
    if (!store || !output)
        return CN_DEVICE_ID_INVALID_ARGUMENT;
    if (output_capacity < CN_DEVICE_ID_TEXT_CAPACITY)
        return CN_DEVICE_ID_BUFFER_TOO_SMALL;
    if (!store_valid(store))
        return CN_DEVICE_ID_INVALID_ARGUMENT;

    result = read_existing(store, output, error_out);
    if (result == CN_DEVICE_ID_OK)
        return result;
    if (result != CN_DEVICE_ID_NOT_FOUND)
        return result;

    memset(random_bytes, 0, sizeof random_bytes);
    entropy_result = (store->entropy ? store->entropy : production_entropy)(
        store->entropy_context, random_bytes, sizeof random_bytes);
    if (entropy_result != 0)
        return CN_DEVICE_ID_ENTROPY_FAILED;
    size = serialize(random_bytes, id, file);
    if (!size)
        return CN_DEVICE_ID_IO_ERROR;
    result = write_new(store, file, size, error_out);
    if (result == CN_DEVICE_ID_IO_ERROR && *error_out == EEXIST) {
        *error_out = 0;
        return read_existing(store, output, error_out);
    }
    if (result != CN_DEVICE_ID_CREATED)
        return result;
    memcpy(output, id, sizeof id);
    return result;
}

const char *cn_device_identity_result_name(cn_device_identity_result result)
{
    static const char *const names[CN_DEVICE_ID_RESULT_COUNT] = {
        "ok", "created", "invalid", "buffer-too-small", "not-found",
        "malformed", "unsupported-version", "bad-checksum", "entropy-failed",
        "path-too-long", "not-directory", "symlink", "non-regular",
        "permission-denied", "read-only", "no-space", "io-error",
        "durability-uncertain"
    };
    return result >= 0 && result < CN_DEVICE_ID_RESULT_COUNT
               ? names[result] : "unknown";
}
