#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "credentials/credential_store.h"

#define FILE_NAME "credentials"
#define TEMP_SUFFIX ".tmp."

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (length--)
        *p++ = 0;
}

void cn_credentials_clear(cn_credentials *credentials)
{
    if (credentials)
        clear_bytes(credentials, sizeof *credentials);
}

static void save_errno(int *out, int value)
{
    if (out)
        *out = value;
}

static size_t bounded_length(const char *s, size_t capacity)
{
    size_t i;
    if (!s)
        return capacity;
    for (i = 0; i < capacity; ++i)
        if (!s[i])
            return i;
    return capacity;
}

static cn_credential_result fs_error(int error)
{
    switch (error) {
    case ENOENT: return CN_CREDENTIAL_NOT_FOUND;
    case ENOTDIR: return CN_CREDENTIAL_NOT_DIRECTORY;
    case ELOOP: return CN_CREDENTIAL_SYMLINK;
    case ENAMETOOLONG: return CN_CREDENTIAL_PATH_TOO_LONG;
    case EACCES: case EPERM: return CN_CREDENTIAL_PERMISSION_DENIED;
    case EROFS: return CN_CREDENTIAL_READ_ONLY;
    case ENOSPC: return CN_CREDENTIAL_NO_SPACE;
#ifdef EDQUOT
    case EDQUOT: return CN_CREDENTIAL_NO_SPACE;
#endif
    default: return CN_CREDENTIAL_IO_ERROR;
    }
}

static int valid_text(const char *text, size_t max)
{
    size_t i, len = bounded_length(text, max + 1);
    if (len == 0 || len > max)
        return 0;
    for (i = 0; i < len; ++i)
        if ((unsigned char)text[i] < 0x20 || (unsigned char)text[i] == 0x7f)
            return 0;
    return 1;
}

static cn_credential_result inspect_directory(const char *path, int *error)
{
    struct stat st;
    if (lstat(path, &st) != 0) {
        save_errno(error, errno);
        return fs_error(errno);
    }
    if (S_ISLNK(st.st_mode)) return CN_CREDENTIAL_SYMLINK;
    return S_ISDIR(st.st_mode) ? CN_CREDENTIAL_OK : CN_CREDENTIAL_NOT_DIRECTORY;
}

static int store_valid(const cn_credential_store *store)
{
    size_t len;
    return store && store->directory_length > 1 &&
           store->directory_length < CN_CREDENTIAL_PATH_CAPACITY - 48 &&
           (len = bounded_length(store->directory, sizeof store->directory)) ==
               store->directory_length &&
           store->directory[0] == '/' && store->directory[len - 1] != '/';
}

static void final_path(const cn_credential_store *store,
                       char path[CN_CREDENTIAL_PATH_CAPACITY])
{
    memcpy(path, store->directory, store->directory_length);
    memcpy(path + store->directory_length, "/" FILE_NAME,
           sizeof "/" FILE_NAME);
}

cn_credential_result cn_credential_store_init(cn_credential_store *store,
                                               const char *directory,
                                               int *system_errno)
{
    size_t len, i, component;
    cn_credential_result result;
    save_errno(system_errno, 0);
    if (!store) return CN_CREDENTIAL_INVALID;
    memset(store, 0, sizeof *store);
    len = bounded_length(directory, CN_CREDENTIAL_PATH_CAPACITY);
    if (len >= CN_CREDENTIAL_PATH_CAPACITY - 48)
        return CN_CREDENTIAL_PATH_TOO_LONG;
    if (len < 2 || directory[0] != '/') return CN_CREDENTIAL_INVALID;
    while (len > 1 && directory[len - 1] == '/') --len;
    component = 1;
    for (i = 1; i <= len; ++i) {
        if (i == len || directory[i] == '/') {
            if (i == component || (i - component == 1 && directory[i - 1] == '.') ||
                (i - component == 2 && directory[i - 2] == '.' && directory[i - 1] == '.'))
                return CN_CREDENTIAL_INVALID;
            component = i + 1;
        }
    }
    memcpy(store->directory, directory, len);
    store->directory[len] = 0;
    store->directory_length = len;
    result = inspect_directory(store->directory, system_errno);
    if (result != CN_CREDENTIAL_OK) memset(store, 0, sizeof *store);
    return result;
}

static cn_credential_result inspect_file(const char *path, struct stat *st,
                                          int *system_errno)
{
    if (lstat(path, st) != 0) {
        if (errno == ENOENT) return CN_CREDENTIAL_MISSING;
        save_errno(system_errno, errno);
        return fs_error(errno);
    }
    if (S_ISLNK(st->st_mode)) return CN_CREDENTIAL_SYMLINK;
    return S_ISREG(st->st_mode) ? CN_CREDENTIAL_OK : CN_CREDENTIAL_NON_REGULAR;
}

static uint32_t crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    int bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
}

static int next_line(const unsigned char *data, size_t size, size_t *offset,
                     const unsigned char **line, size_t *length)
{
    size_t i = *offset;
    if (i >= size) return 0;
    while (i < size && data[i] != '\n') ++i;
    if (i == size) return 0;
    *line = data + *offset;
    *length = i - *offset;
    *offset = i + 1;
    return 1;
}

static int field(const unsigned char *line, size_t length, const char *name,
                 const unsigned char **value, size_t *value_length)
{
    size_t n = strlen(name);
    if (length < n + 1 || memcmp(line, name, n) || line[n] != '=') return 0;
    *value = line + n + 1;
    *value_length = length - n - 1;
    return 1;
}

static cn_credential_result parse(const unsigned char *data, size_t size,
                                   cn_credentials *output)
{
    cn_credentials parsed = {{0}, {0}};
    const unsigned char *lines[4], *value;
    size_t lengths[4], offset = 0, n, i;
    uint32_t actual = 0;
    cn_credential_result result = CN_CREDENTIAL_CORRUPT;
    if (!size || size > CN_CREDENTIAL_FILE_MAX_BYTES || memchr(data, 0, size))
        return result;
    for (i = 0; i < 4; ++i)
        if (!next_line(data, size, &offset, &lines[i], &lengths[i]))
            goto done;
    if (offset != size || !field(lines[0], lengths[0], "crossnook-credentials", &value, &n))
        goto done;
    if (!n || (n > 1 && value[0] == '0')) goto done;
    for (i = 0; i < n; ++i)
        if (value[i] < '0' || value[i] > '9') goto done;
    if (n != 1 || value[0] != '1') {
        result = CN_CREDENTIAL_UNSUPPORTED_VERSION;
        goto done;
    }
    if (!field(lines[1], lengths[1], "username", &value, &n) ||
        !n || n > CN_CREDENTIAL_USERNAME_MAX) goto done;
    memcpy(parsed.username, value, n);
    if (!field(lines[2], lengths[2], "userkey", &value, &n) ||
        !n || n > CN_CREDENTIAL_USERKEY_MAX) goto done;
    memcpy(parsed.userkey, value, n);
    if (!valid_text(parsed.username, CN_CREDENTIAL_USERNAME_MAX) ||
        !valid_text(parsed.userkey, CN_CREDENTIAL_USERKEY_MAX) ||
        !field(lines[3], lengths[3], "crc32", &value, &n) || n != 8)
        goto done;
    for (i = 0; i < n; ++i) {
        unsigned digit;
        if (value[i] >= '0' && value[i] <= '9') digit = value[i] - '0';
        else if (value[i] >= 'a' && value[i] <= 'f') digit = value[i] - 'a' + 10u;
        else goto done;
        actual = (actual << 4) | digit;
    }
    if (actual != crc32(data, (size_t)(lines[3] - data))) goto done;
    *output = parsed;
    result = CN_CREDENTIAL_OK;
done:
    cn_credentials_clear(&parsed);
    return result;
}

static cn_credential_result serialize(const cn_credentials *credentials,
                                       unsigned char *data, size_t *size)
{
    static const char hex[] = "0123456789abcdef";
    static const char prefix[] = "crossnook-credentials=1\nusername=";
    static const char middle[] = "\nuserkey=";
    static const char end[] = "\ncrc32=";
    size_t u, k, i, pos = 0;
    uint32_t crc;
    if (!credentials || !valid_text(credentials->username, CN_CREDENTIAL_USERNAME_MAX) ||
        !valid_text(credentials->userkey, CN_CREDENTIAL_USERKEY_MAX))
        return CN_CREDENTIAL_INVALID;
    u = strlen(credentials->username); k = strlen(credentials->userkey);
#define APPEND(src, n) do { memcpy(data + pos, (src), (n)); pos += (n); } while (0)
    APPEND(prefix, sizeof prefix - 1);
    APPEND(credentials->username, u);
    APPEND(middle, sizeof middle - 1);
    APPEND(credentials->userkey, k);
    APPEND("\n", 1);
    crc = crc32(data, pos);
    APPEND(end + 1, sizeof end - 2);
    for (i = 0; i < 8; ++i)
        data[pos++] = (unsigned char)hex[(crc >> (28 - 4 * i)) & 15];
    APPEND("\n", 1);
#undef APPEND
    *size = pos;
    return CN_CREDENTIAL_OK;
}

cn_credential_result cn_credential_store_load(const cn_credential_store *store,
                                               cn_credentials *credentials,
                                               int *system_errno)
{
    unsigned char data[CN_CREDENTIAL_FILE_MAX_BYTES];
    char path[CN_CREDENTIAL_PATH_CAPACITY];
    struct stat before, opened;
    cn_credential_result result;
    size_t done = 0;
    int fd = -1, error;
    ssize_t n;
    unsigned char extra;
    save_errno(system_errno, 0);
    if (!credentials) return CN_CREDENTIAL_INVALID;
    cn_credentials_clear(credentials);
    if (!store_valid(store)) return CN_CREDENTIAL_INVALID;
    result = inspect_directory(store->directory, system_errno);
    if (result != CN_CREDENTIAL_OK) return result;
    final_path(store, path);
    result = inspect_file(path, &before, system_errno);
    if (result != CN_CREDENTIAL_OK) return result;
    if (before.st_size <= 0 || before.st_size > CN_CREDENTIAL_FILE_MAX_BYTES)
        return CN_CREDENTIAL_CORRUPT;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { save_errno(system_errno, errno); return fs_error(errno); }
    if (fstat(fd, &opened) != 0) {
        error = errno; save_errno(system_errno, error); result = fs_error(error); goto done;
    }
    if (!S_ISREG(opened.st_mode)) { result = CN_CREDENTIAL_NON_REGULAR; goto done; }
    if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino) {
        result = CN_CREDENTIAL_IO_ERROR; goto done;
    }
    if (opened.st_size <= 0 || opened.st_size > CN_CREDENTIAL_FILE_MAX_BYTES) {
        result = CN_CREDENTIAL_CORRUPT; goto done;
    }
    while (done < (size_t)opened.st_size) {
        n = read(fd, data + done, (size_t)opened.st_size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            if (n < 0) { save_errno(system_errno, errno); result = fs_error(errno); }
            else result = CN_CREDENTIAL_CORRUPT;
            goto done;
        }
        done += (size_t)n;
    }
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    if (n != 0) {
        if (n < 0) { save_errno(system_errno, errno); result = fs_error(errno); }
        else result = CN_CREDENTIAL_CORRUPT;
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1; save_errno(system_errno, errno); result = fs_error(errno); goto done;
    }
    fd = -1;
    result = parse(data, done, credentials);
done:
    if (fd >= 0) (void)close(fd);
    clear_bytes(data, sizeof data);
    if (result != CN_CREDENTIAL_OK) cn_credentials_clear(credentials);
    return result;
}

cn_credential_result cn_credential_store_save(const cn_credential_store *store,
                                               const cn_credentials *credentials,
                                               int *system_errno)
{
    unsigned char data[CN_CREDENTIAL_FILE_MAX_BYTES];
    char path[CN_CREDENTIAL_PATH_CAPACITY], temp[CN_CREDENTIAL_PATH_CAPACITY];
    struct stat st, dir_before, dir_opened;
    cn_credential_result result;
    static unsigned sequence;
    size_t size = 0, done = 0;
    int fd = -1, dirfd = -1, created = 0, error, tries;
    ssize_t n;
    save_errno(system_errno, 0);
    if (!store_valid(store)) return CN_CREDENTIAL_INVALID;
    result = serialize(credentials, data, &size);
    if (result != CN_CREDENTIAL_OK) goto done;
    result = inspect_directory(store->directory, system_errno);
    if (result != CN_CREDENTIAL_OK) goto done;
    if (lstat(store->directory, &dir_before) != 0) {
        save_errno(system_errno, errno); result = fs_error(errno); goto done;
    }
    final_path(store, path);
    result = inspect_file(path, &st, system_errno);
    if (result != CN_CREDENTIAL_OK && result != CN_CREDENTIAL_MISSING) goto done;
    save_errno(system_errno, 0);
    for (tries = 0; tries < 32; ++tries) {
        memcpy(temp, store->directory, store->directory_length);
        if (snprintf(temp + store->directory_length,
                     sizeof temp - store->directory_length,
                     "/" FILE_NAME TEMP_SUFFIX "%ld.%u",
                     (long)getpid(), ++sequence) >=
            (int)(sizeof temp - store->directory_length)) {
            result = CN_CREDENTIAL_PATH_TOO_LONG; goto done;
        }
        fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (fd >= 0) break;
        if (errno != EEXIST) {
            save_errno(system_errno, errno); result = fs_error(errno); goto done;
        }
    }
    if (fd < 0) { result = CN_CREDENTIAL_IO_ERROR; goto done; }
    created = 1;
    while (done < size) {
        n = write(fd, data + done, size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            error = n < 0 ? errno : EIO;
            save_errno(system_errno, error); result = fs_error(error); goto done;
        }
        done += (size_t)n;
    }
    if (fsync(fd) != 0) {
        save_errno(system_errno, errno); result = fs_error(errno); goto done;
    }
    if (close(fd) != 0) {
        fd = -1; save_errno(system_errno, errno); result = fs_error(errno); goto done;
    }
    fd = -1;
    /* Refuse a final entry that became a symlink or non-regular file. */
    result = inspect_file(path, &st, system_errno);
    if (result != CN_CREDENTIAL_OK && result != CN_CREDENTIAL_MISSING) goto done;
    save_errno(system_errno, 0);
    if (rename(temp, path) != 0) {
        save_errno(system_errno, errno); result = fs_error(errno); goto done;
    }
    created = 0;
    result = CN_CREDENTIAL_DURABILITY_UNCERTAIN;
    dirfd = open(store->directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (dirfd < 0) { save_errno(system_errno, errno); goto done; }
    if (fstat(dirfd, &dir_opened) != 0) {
        save_errno(system_errno, errno); goto done;
    }
    if (!S_ISDIR(dir_opened.st_mode) || dir_before.st_dev != dir_opened.st_dev ||
        dir_before.st_ino != dir_opened.st_ino) goto done;
    if (fsync(dirfd) != 0) { save_errno(system_errno, errno); goto done; }
    if (close(dirfd) != 0) {
        dirfd = -1; save_errno(system_errno, errno); goto done;
    }
    dirfd = -1;
    result = CN_CREDENTIAL_OK;
done:
    if (fd >= 0) (void)close(fd);
    if (dirfd >= 0) (void)close(dirfd);
    if (created && unlink(temp) != 0 && errno != ENOENT &&
        result != CN_CREDENTIAL_DURABILITY_UNCERTAIN) {
        save_errno(system_errno, errno); result = fs_error(errno);
    }
    clear_bytes(data, sizeof data);
    return result;
}

const char *cn_credential_result_name(cn_credential_result result)
{
    static const char *const names[CN_CREDENTIAL_RESULT_COUNT] = {
        "ok", "missing", "invalid", "corrupt", "unsupported-version",
        "path-too-long", "not-found", "not-directory", "symlink",
        "non-regular", "permission-denied", "read-only", "no-space",
        "io-error", "durability-uncertain"
    };
    return result >= 0 && result < CN_CREDENTIAL_RESULT_COUNT ? names[result] : "unknown";
}
