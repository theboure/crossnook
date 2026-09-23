#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "settings/settings_store.h"

#define SETTINGS_FILE_NAME "settings.conf"
typedef struct settings_line {
    const unsigned char *data;
    size_t length;
} settings_line;

static void set_system_errno(int *system_errno, int value)
{
    if (system_errno)
        *system_errno = value;
}

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t i;
    if (!text)
        return capacity;
    for (i = 0; i < capacity; ++i) {
        if (text[i] == '\0')
            return i;
    }
    return capacity;
}

static cn_settings_result filesystem_error(int error)
{
    switch (error) {
    case ENOENT:
        return CN_SETTINGS_NOT_FOUND;
    case ENOTDIR:
        return CN_SETTINGS_NOT_DIRECTORY;
    case ELOOP:
        return CN_SETTINGS_SYMLINK;
    case EACCES:
    case EPERM:
        return CN_SETTINGS_PERMISSION_DENIED;
    case EROFS:
        return CN_SETTINGS_READ_ONLY;
    case ENOSPC:
#ifdef EDQUOT
    case EDQUOT:
#endif
        return CN_SETTINGS_NO_SPACE;
    case ENAMETOOLONG:
        return CN_SETTINGS_PATH_TOO_LONG;
    default:
        return CN_SETTINGS_IO_ERROR;
    }
}

static int utf8_device_name_valid(const unsigned char *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        uint32_t codepoint;
        unsigned char first = text[i++];
        int remaining;
        uint32_t minimum;
        if (first < 0x80) {
            codepoint = first;
            remaining = 0;
            minimum = 0;
        } else if ((first & 0xe0) == 0xc0) {
            codepoint = first & 0x1f;
            remaining = 1;
            minimum = 0x80;
        } else if ((first & 0xf0) == 0xe0) {
            codepoint = first & 0x0f;
            remaining = 2;
            minimum = 0x800;
        } else if ((first & 0xf8) == 0xf0) {
            codepoint = first & 0x07;
            remaining = 3;
            minimum = 0x10000;
        } else {
            return 0;
        }
        if (i + (size_t)remaining > length)
            return 0;
        while (remaining-- > 0) {
            unsigned char continuation = text[i++];
            if ((continuation & 0xc0) != 0x80)
                return 0;
            codepoint = (codepoint << 6) | (continuation & 0x3f);
        }
        if (codepoint < minimum || codepoint > 0x10ffff ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f))
            return 0;
    }
    return 1;
}

static int decimal_port_valid(const char *begin, const char *end)
{
    unsigned value = 0;
    const char *p;
    if (begin == end || (size_t)(end - begin) > 5)
        return 0;
    for (p = begin; p < end; ++p) {
        if (*p < '0' || *p > '9')
            return 0;
        value = value * 10u + (unsigned)(*p - '0');
    }
    return value > 0 && value <= 65535;
}

static int base_url_valid(const char *url, size_t length)
{
    const char *authority;
    const char *authority_end;
    const char *colon = NULL;
    const char *p;
    size_t host_length;
    size_t path_length;

    if (length < sizeof "https://a" - 1 ||
        memcmp(url, "https://", sizeof "https://" - 1) != 0)
        return 0;
    for (p = url; p < url + length; ++p) {
        unsigned char byte = (unsigned char)*p;
        if (byte < 0x21 || byte > 0x7e || byte == '@' ||
            byte == '?' || byte == '#' || byte == '\\')
            return 0;
    }

    authority = url + (sizeof "https://" - 1);
    authority_end = authority;
    while (authority_end < url + length && *authority_end != '/') {
        if (*authority_end == ':') {
            if (colon)
                return 0;
            colon = authority_end;
        }
        ++authority_end;
    }
    host_length = (size_t)((colon ? colon : authority_end) - authority);
    if (host_length == 0 || host_length > 255)
        return 0;
    if (colon && !decimal_port_valid(colon + 1, authority_end))
        return 0;
    path_length = (size_t)((url + length) - authority_end);
    return path_length <= 1024;
}

void cn_settings_defaults(cn_settings *settings)
{
    if (!settings)
        return;
    memset(settings, 0, sizeof *settings);
    memcpy(settings->kosync_device_name, "CrossNook", sizeof "CrossNook");
}

cn_settings_result cn_settings_validate(const cn_settings *settings)
{
    size_t url_length;
    size_t device_length;
    if (!settings)
        return CN_SETTINGS_INVALID_ARGUMENT;
    if (settings->kosync_enabled != 0 && settings->kosync_enabled != 1)
        return CN_SETTINGS_INVALID_SETTINGS;
    url_length = bounded_length(settings->kosync_base_url,
                                sizeof settings->kosync_base_url);
    device_length = bounded_length(settings->kosync_device_name,
                                   sizeof settings->kosync_device_name);
    if (url_length >= sizeof settings->kosync_base_url ||
        device_length == 0 ||
        device_length >= sizeof settings->kosync_device_name ||
        !utf8_device_name_valid(
            (const unsigned char *)settings->kosync_device_name,
            device_length))
        return CN_SETTINGS_INVALID_SETTINGS;
    if (url_length == 0)
        return settings->kosync_enabled ? CN_SETTINGS_INVALID_SETTINGS
                                        : CN_SETTINGS_OK;
    return base_url_valid(settings->kosync_base_url, url_length)
               ? CN_SETTINGS_OK : CN_SETTINGS_INVALID_SETTINGS;
}

static int store_valid(const cn_settings_store *store)
{
    size_t length;
    if (!store || store->config_directory_length == 0 ||
        store->config_directory_length >= CN_SETTINGS_PATH_CAPACITY)
        return 0;
    length = bounded_length(store->config_directory,
                            sizeof store->config_directory);
    return length == store->config_directory_length &&
           store->config_directory[0] == '/';
}

static cn_settings_result inspect_config_directory(
    const cn_settings_store *store, int *system_errno)
{
    struct stat st;
    int error;
    if (lstat(store->config_directory, &st) != 0) {
        error = errno;
        set_system_errno(system_errno, error);
        return filesystem_error(error);
    }
    if (S_ISLNK(st.st_mode))
        return CN_SETTINGS_SYMLINK;
    if (!S_ISDIR(st.st_mode))
        return CN_SETTINGS_NOT_DIRECTORY;
    return CN_SETTINGS_OK;
}

static cn_settings_result build_paths(const cn_settings_store *store,
                                      char final_path[CN_SETTINGS_PATH_CAPACITY],
                                      char temporary[CN_SETTINGS_PATH_CAPACITY])
{
    const char *separator;
    int written;
    size_t final_length;
    if (!store_valid(store))
        return CN_SETTINGS_INVALID_ARGUMENT;
    separator = store->config_directory_length == 1 ? "" : "/";
    final_length = store->config_directory_length + strlen(separator) +
                   (sizeof SETTINGS_FILE_NAME - 1);
    if (final_length >= CN_SETTINGS_PATH_CAPACITY)
        return CN_SETTINGS_PATH_TOO_LONG;
    written = snprintf(final_path, CN_SETTINGS_PATH_CAPACITY, "%s%s%s",
                       store->config_directory, separator,
                       SETTINGS_FILE_NAME);
    if (written < 0 || written >= CN_SETTINGS_PATH_CAPACITY)
        return CN_SETTINGS_PATH_TOO_LONG;
    written = snprintf(temporary, CN_SETTINGS_PATH_CAPACITY,
                       "%s.tmp.%ld", final_path, (long)getpid());
    if (written < 0 || written >= CN_SETTINGS_PATH_CAPACITY) {
        final_path[0] = '\0';
        temporary[0] = '\0';
        return CN_SETTINGS_PATH_TOO_LONG;
    }
    return CN_SETTINGS_OK;
}

cn_settings_result cn_settings_store_init(cn_settings_store *store,
                                          const char *config_directory,
                                          int *system_errno)
{
    cn_settings_store candidate;
    cn_settings_result result;
    size_t length;
    char final_path[CN_SETTINGS_PATH_CAPACITY];
    char temporary[CN_SETTINGS_PATH_CAPACITY];

    set_system_errno(system_errno, 0);
    if (!store)
        return CN_SETTINGS_INVALID_ARGUMENT;
    memset(store, 0, sizeof *store);
    length = bounded_length(config_directory, CN_SETTINGS_PATH_CAPACITY);
    if (!config_directory || length == 0 || config_directory[0] != '/')
        return CN_SETTINGS_INVALID_ARGUMENT;
    if (length >= CN_SETTINGS_PATH_CAPACITY)
        return CN_SETTINGS_PATH_TOO_LONG;
    while (length > 1 && config_directory[length - 1] == '/')
        --length;
    memset(&candidate, 0, sizeof candidate);
    memcpy(candidate.config_directory, config_directory, length);
    candidate.config_directory[length] = '\0';
    candidate.config_directory_length = length;
    result = build_paths(&candidate, final_path, temporary);
    if (result != CN_SETTINGS_OK)
        return result;
    result = inspect_config_directory(&candidate, system_errno);
    if (result != CN_SETTINGS_OK)
        return result;
    *store = candidate;
    return CN_SETTINGS_OK;
}

static uint32_t settings_crc32(const unsigned char *data, size_t length)
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

static int next_line(const unsigned char *data, size_t length,
                     size_t *offset, settings_line *line)
{
    size_t start = *offset;
    size_t end = start;
    if (start >= length)
        return 0;
    while (end < length && data[end] != '\n')
        ++end;
    if (end == length)
        return 0;
    line->data = data + start;
    line->length = end - start;
    *offset = end + 1;
    return 1;
}

static int line_value(const settings_line *line, const char *key,
                      const unsigned char **value, size_t *value_length)
{
    size_t key_length = strlen(key);
    if (line->length < key_length + 1 ||
        memcmp(line->data, key, key_length) != 0 ||
        line->data[key_length] != '=')
        return 0;
    *value = line->data + key_length + 1;
    *value_length = line->length - key_length - 1;
    return 1;
}

static int parse_version(const unsigned char *value, size_t length,
                         unsigned *version)
{
    size_t i;
    if (length == 0 || (length > 1 && value[0] == '0'))
        return 0;
    for (i = 0; i < length; ++i) {
        if (value[i] < '0' || value[i] > '9')
            return 0;
    }
    *version = length == 1 && value[0] == '1'
                   ? CN_SETTINGS_FORMAT_VERSION
                   : CN_SETTINGS_FORMAT_VERSION + 1;
    return 1;
}

static int parse_crc(const unsigned char *value, size_t length,
                     uint32_t *crc)
{
    size_t i;
    uint32_t parsed = 0;
    if (length != 8)
        return 0;
    for (i = 0; i < length; ++i) {
        unsigned digit;
        if (value[i] >= '0' && value[i] <= '9')
            digit = (unsigned)(value[i] - '0');
        else if (value[i] >= 'a' && value[i] <= 'f')
            digit = (unsigned)(value[i] - 'a') + 10u;
        else
            return 0;
        parsed = (parsed << 4) | digit;
    }
    *crc = parsed;
    return 1;
}

static cn_settings_result parse_settings(const unsigned char *data,
                                         size_t length,
                                         cn_settings *settings)
{
    cn_settings parsed;
    settings_line lines[5];
    const unsigned char *value;
    size_t value_length;
    size_t offset = 0;
    size_t crc_offset;
    unsigned version;
    uint32_t stored_crc;
    size_t i;

    if (length == 0 || length > CN_SETTINGS_FILE_MAX_BYTES ||
        memchr(data, '\0', length) != NULL)
        return CN_SETTINGS_CORRUPT;
    if (!next_line(data, length, &offset, &lines[0]))
        return CN_SETTINGS_CORRUPT;
    if (!line_value(&lines[0], "crossnook-settings", &value,
                    &value_length) ||
        !parse_version(value, value_length, &version))
        return CN_SETTINGS_CORRUPT;
    if (version != CN_SETTINGS_FORMAT_VERSION)
        return CN_SETTINGS_UNSUPPORTED_VERSION;
    for (i = 1; i < 5; ++i) {
        if (!next_line(data, length, &offset, &lines[i]))
            return CN_SETTINGS_CORRUPT;
    }
    if (offset != length)
        return CN_SETTINGS_CORRUPT;
    if (!line_value(&lines[1], "kosync.enabled", &value, &value_length))
        return CN_SETTINGS_CORRUPT;
    cn_settings_defaults(&parsed);
    if (value_length == sizeof "true" - 1 &&
        memcmp(value, "true", value_length) == 0)
        parsed.kosync_enabled = 1;
    else if (value_length == sizeof "false" - 1 &&
             memcmp(value, "false", value_length) == 0)
        parsed.kosync_enabled = 0;
    else
        return CN_SETTINGS_CORRUPT;
    if (!line_value(&lines[2], "kosync.base_url", &value, &value_length) ||
        value_length >= sizeof parsed.kosync_base_url)
        return CN_SETTINGS_CORRUPT;
    memcpy(parsed.kosync_base_url, value, value_length);
    parsed.kosync_base_url[value_length] = '\0';
    if (!line_value(&lines[3], "kosync.device_name", &value,
                    &value_length) ||
        value_length >= sizeof parsed.kosync_device_name)
        return CN_SETTINGS_CORRUPT;
    memcpy(parsed.kosync_device_name, value, value_length);
    parsed.kosync_device_name[value_length] = '\0';
    if (!line_value(&lines[4], "crc32", &value, &value_length) ||
        !parse_crc(value, value_length, &stored_crc))
        return CN_SETTINGS_CORRUPT;
    crc_offset = (size_t)(lines[4].data - data);
    if (settings_crc32(data, crc_offset) != stored_crc ||
        cn_settings_validate(&parsed) != CN_SETTINGS_OK)
        return CN_SETTINGS_CORRUPT;
    *settings = parsed;
    return CN_SETTINGS_OK;
}

static cn_settings_result inspect_settings_file(const char *path,
                                                struct stat *st,
                                                int missing_is_settings,
                                                int *system_errno)
{
    int error;
    if (lstat(path, st) != 0) {
        error = errno;
        if (missing_is_settings && error == ENOENT) {
            set_system_errno(system_errno, error);
            return CN_SETTINGS_MISSING;
        }
        set_system_errno(system_errno, error);
        return filesystem_error(error);
    }
    if (S_ISLNK(st->st_mode))
        return CN_SETTINGS_SYMLINK;
    if (!S_ISREG(st->st_mode))
        return CN_SETTINGS_NON_REGULAR;
    return CN_SETTINGS_OK;
}

cn_settings_result cn_settings_load(const cn_settings_store *store,
                                    cn_settings *settings,
                                    int *system_errno)
{
    unsigned char data[CN_SETTINGS_FILE_MAX_BYTES];
    cn_settings parsed;
    cn_settings_result result;
    struct stat before;
    struct stat opened;
    char path[CN_SETTINGS_PATH_CAPACITY];
    char temporary[CN_SETTINGS_PATH_CAPACITY];
    size_t done = 0;
    int fd = -1;
    int error;
    unsigned char extra;
    ssize_t count;

    set_system_errno(system_errno, 0);
    if (!store || !settings)
        return CN_SETTINGS_INVALID_ARGUMENT;
    cn_settings_defaults(settings);
    if (!store_valid(store))
        return CN_SETTINGS_INVALID_ARGUMENT;
    result = inspect_config_directory(store, system_errno);
    if (result != CN_SETTINGS_OK)
        return result;
    result = build_paths(store, path, temporary);
    if (result != CN_SETTINGS_OK)
        return result;
    result = inspect_settings_file(path, &before, 1, system_errno);
    if (result != CN_SETTINGS_OK)
        return result;
    if (before.st_size <= 0 || before.st_size > CN_SETTINGS_FILE_MAX_BYTES)
        return CN_SETTINGS_CORRUPT;

    fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        error = errno;
        set_system_errno(system_errno, error);
        return filesystem_error(error);
    }
    if (fstat(fd, &opened) != 0) {
        error = errno;
        set_system_errno(system_errno, error);
        (void)close(fd);
        return filesystem_error(error);
    }
    if (!S_ISREG(opened.st_mode)) {
        (void)close(fd);
        return CN_SETTINGS_NON_REGULAR;
    }
    if (opened.st_size <= 0 || opened.st_size > CN_SETTINGS_FILE_MAX_BYTES) {
        (void)close(fd);
        return CN_SETTINGS_CORRUPT;
    }
    if (opened.st_dev != before.st_dev || opened.st_ino != before.st_ino) {
        (void)close(fd);
        return CN_SETTINGS_IO_ERROR;
    }
    while (done < (size_t)opened.st_size) {
        count = read(fd, data + done, (size_t)opened.st_size - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            error = errno;
            set_system_errno(system_errno, error);
            (void)close(fd);
            return filesystem_error(error);
        }
        if (count == 0) {
            (void)close(fd);
            return CN_SETTINGS_CORRUPT;
        }
        done += (size_t)count;
    }
    do {
        count = read(fd, &extra, 1);
    } while (count < 0 && errno == EINTR);
    if (count < 0) {
        error = errno;
        set_system_errno(system_errno, error);
        (void)close(fd);
        return filesystem_error(error);
    }
    if (count != 0) {
        (void)close(fd);
        return CN_SETTINGS_CORRUPT;
    }
    if (close(fd) != 0) {
        error = errno;
        set_system_errno(system_errno, error);
        return filesystem_error(error);
    }
    fd = -1;
    cn_settings_defaults(&parsed);
    result = parse_settings(data, done, &parsed);
    if (result == CN_SETTINGS_OK)
        *settings = parsed;
    return result;
}

typedef struct settings_writer {
    unsigned char *data;
    size_t capacity;
    size_t length;
} settings_writer;

static int writer_append(settings_writer *writer, const void *data,
                         size_t length)
{
    if (length > writer->capacity - writer->length)
        return 0;
    memcpy(writer->data + writer->length, data, length);
    writer->length += length;
    return 1;
}

static int writer_text(settings_writer *writer, const char *text)
{
    return writer_append(writer, text, strlen(text));
}

static cn_settings_result serialize_settings(
    const cn_settings *settings,
    unsigned char data[CN_SETTINGS_FILE_MAX_BYTES], size_t *length)
{
    static const char hex[] = "0123456789abcdef";
    settings_writer writer;
    uint32_t crc;
    char crc_line[16] = "crc32=00000000\n";
    int shift;

    writer.data = data;
    writer.capacity = CN_SETTINGS_FILE_MAX_BYTES;
    writer.length = 0;
    if (!writer_text(&writer, "crossnook-settings=1\n") ||
        !writer_text(&writer, "kosync.enabled=") ||
        !writer_text(&writer, settings->kosync_enabled ? "true\n" :
                                                        "false\n") ||
        !writer_text(&writer, "kosync.base_url=") ||
        !writer_text(&writer, settings->kosync_base_url) ||
        !writer_text(&writer, "\n") ||
        !writer_text(&writer, "kosync.device_name=") ||
        !writer_text(&writer, settings->kosync_device_name) ||
        !writer_text(&writer, "\n"))
        return CN_SETTINGS_INVALID_SETTINGS;
    crc = settings_crc32(data, writer.length);
    for (shift = 28; shift >= 0; shift -= 4)
        crc_line[6 + (28 - shift) / 4] = hex[(crc >> shift) & 0x0f];
    if (!writer_append(&writer, crc_line, sizeof crc_line - 1))
        return CN_SETTINGS_INVALID_SETTINGS;
    *length = writer.length;
    return CN_SETTINGS_OK;
}

static cn_settings_result write_all(int fd, const unsigned char *data,
                                    size_t length, int *system_errno)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(fd, data + done, length - done);
        int error;
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            error = count < 0 ? errno : EIO;
            set_system_errno(system_errno, error);
            return filesystem_error(error);
        }
        done += (size_t)count;
    }
    return CN_SETTINGS_OK;
}

cn_settings_result cn_settings_save(const cn_settings_store *store,
                                    const cn_settings *settings,
                                    int *system_errno)
{
    unsigned char data[CN_SETTINGS_FILE_MAX_BYTES];
    char path[CN_SETTINGS_PATH_CAPACITY];
    char temporary[CN_SETTINGS_PATH_CAPACITY];
    cn_settings_result result;
    struct stat st;
    size_t length;
    int fd = -1;
    int directory_fd = -1;
    int error;
    int renamed = 0;

    set_system_errno(system_errno, 0);
    if (!store || !settings)
        return CN_SETTINGS_INVALID_ARGUMENT;
    if (!store_valid(store))
        return CN_SETTINGS_INVALID_ARGUMENT;
    result = cn_settings_validate(settings);
    if (result != CN_SETTINGS_OK)
        return result;
    result = serialize_settings(settings, data, &length);
    if (result != CN_SETTINGS_OK)
        return result;
    result = inspect_config_directory(store, system_errno);
    if (result != CN_SETTINGS_OK)
        return result;
    result = build_paths(store, path, temporary);
    if (result != CN_SETTINGS_OK)
        return result;
    result = inspect_settings_file(path, &st, 1, system_errno);
    if (result != CN_SETTINGS_OK && result != CN_SETTINGS_MISSING)
        return result;
    set_system_errno(system_errno, 0);

    if (unlink(temporary) != 0 && errno != ENOENT) {
        error = errno;
        set_system_errno(system_errno, error);
        return filesystem_error(error);
    }
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (fd < 0) {
        error = errno;
        set_system_errno(system_errno, error);
        return filesystem_error(error);
    }
    result = write_all(fd, data, length, system_errno);
    if (result != CN_SETTINGS_OK)
        goto done;
    if (fsync(fd) != 0) {
        error = errno;
        set_system_errno(system_errno, error);
        result = filesystem_error(error);
        goto done;
    }
    if (close(fd) != 0) {
        error = errno;
        fd = -1;
        set_system_errno(system_errno, error);
        result = filesystem_error(error);
        goto done;
    }
    fd = -1;
    if (rename(temporary, path) != 0) {
        error = errno;
        set_system_errno(system_errno, error);
        result = filesystem_error(error);
        goto done;
    }
    renamed = 1;

    directory_fd = open(store->config_directory, O_RDONLY | O_DIRECTORY);
    if (directory_fd < 0) {
        error = errno;
        set_system_errno(system_errno, error);
        return CN_SETTINGS_DURABILITY_UNCERTAIN;
    }
    if (fsync(directory_fd) != 0) {
        error = errno;
        set_system_errno(system_errno, error);
        (void)close(directory_fd);
        return CN_SETTINGS_DURABILITY_UNCERTAIN;
    }
    if (close(directory_fd) != 0) {
        error = errno;
        set_system_errno(system_errno, error);
        return CN_SETTINGS_DURABILITY_UNCERTAIN;
    }
    return CN_SETTINGS_OK;

done:
    if (fd >= 0)
        (void)close(fd);
    if (!renamed && unlink(temporary) != 0 && errno != ENOENT) {
        error = errno;
        set_system_errno(system_errno, error);
        result = filesystem_error(error);
    }
    return result;
}

const char *cn_settings_result_name(cn_settings_result result)
{
    static const char *const names[CN_SETTINGS_RESULT_COUNT] = {
        "ok", "missing", "invalid-argument", "invalid-settings",
        "corrupt", "unsupported-version", "path-too-long", "not-found",
        "not-directory", "symlink", "non-regular", "permission-denied",
        "read-only", "no-space", "io-error", "durability-uncertain"
    };
    if (result < 0 || result >= CN_SETTINGS_RESULT_COUNT)
        return "unknown";
    return names[result];
}
