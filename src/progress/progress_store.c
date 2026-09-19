#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "progress/progress_store.h"

#define PROGRESS_HEADER_BYTES 24
#define PROGRESS_CRC_BYTES 4
#define PROGRESS_STATE_DIR_MAX_BYTES 4096

static const unsigned char PROGRESS_MAGIC[8] = {
    'C', 'N', 'P', 'R', 'O', 'G', '\r', '\n'
};

struct cn_progress_store {
    char *directory;
};

static uint32_t read_u32be(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_u32be(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)(value >> 24);
    p[1] = (unsigned char)(value >> 16);
    p[2] = (unsigned char)(value >> 8);
    p[3] = (unsigned char)value;
}

static uint32_t progress_crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    int bit;
    for (i = 0; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) &
                                (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
}

static int bounded_length(const char *text, size_t maximum, size_t *length)
{
    size_t n;
    if (!text)
        return -1;
    for (n = 0; n <= maximum; ++n) {
        if (text[n] == '\0') {
            if (n == 0)
                return -1;
            *length = n;
            return 0;
        }
    }
    return -1;
}

static int utf8_valid(const unsigned char *text, size_t length)
{
    size_t i = 0;
    while (i < length) {
        unsigned char c = text[i++];
        int continuation;
        unsigned char min_second = 0x80;
        unsigned char max_second = 0xbf;
        if (c == 0)
            return -1;
        if (c <= 0x7f)
            continue;
        if (c >= 0xc2 && c <= 0xdf) {
            continuation = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            continuation = 2;
            if (c == 0xe0)
                min_second = 0xa0;
            else if (c == 0xed)
                max_second = 0x9f;
        } else if (c >= 0xf0 && c <= 0xf4) {
            continuation = 3;
            if (c == 0xf0)
                min_second = 0x90;
            else if (c == 0xf4)
                max_second = 0x8f;
        } else {
            return -1;
        }
        if (i + (size_t)continuation > length ||
            text[i] < min_second || text[i] > max_second)
            return -1;
        ++i;
        while (--continuation > 0) {
            if (text[i] < 0x80 || text[i] > 0xbf)
                return -1;
            ++i;
        }
    }
    return 0;
}

static char *record_path(const cn_progress_store *store,
                         const cn_book_identity *identity)
{
    const char *token = cn_book_identity_token(identity);
    size_t directory_length;
    size_t token_length;
    size_t suffix_length = sizeof CN_PROGRESS_FILE_SUFFIX - 1;
    size_t total;
    int slash;
    char *path;
    if (!store || !store->directory || !token)
        return NULL;
    directory_length = strlen(store->directory);
    token_length = strlen(token);
    slash = store->directory[directory_length - 1] != '/';
    if (directory_length > SIZE_MAX - token_length - suffix_length - 2)
        return NULL;
    total = directory_length + (size_t)slash + token_length + suffix_length + 1;
    path = (char *)malloc(total);
    if (!path)
        return NULL;
    snprintf(path, total, "%s%s%s%s", store->directory, slash ? "/" : "",
             token, CN_PROGRESS_FILE_SUFFIX);
    return path;
}

static int read_all(int fd, unsigned char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = read(fd, data + done, length - done);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static int write_all(int fd, const unsigned char *data, size_t length)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(fd, data + done, length - done);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (count == 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

void cn_progress_record_init(cn_progress_record *record)
{
    if (record)
        cn_reader_position_init(&record->position);
}

void cn_progress_record_clear(cn_progress_record *record)
{
    if (record)
        cn_reader_position_clear(&record->position);
}

cn_progress_result cn_progress_store_open(cn_progress_store **out,
                                          const char *state_directory)
{
    cn_progress_store *store;
    struct stat st;
    size_t length;
    if (!out)
        return CN_PROGRESS_INVALID;
    *out = NULL;
    if (bounded_length(state_directory,
                       PROGRESS_STATE_DIR_MAX_BYTES, &length) != 0)
        return CN_PROGRESS_INVALID;
    if (stat(state_directory, &st) != 0 || !S_ISDIR(st.st_mode))
        return CN_PROGRESS_IO_ERROR;
    store = (cn_progress_store *)calloc(1, sizeof *store);
    if (!store)
        return CN_PROGRESS_NO_MEMORY;
    store->directory = (char *)malloc(length + 1);
    if (!store->directory) {
        free(store);
        return CN_PROGRESS_NO_MEMORY;
    }
    memcpy(store->directory, state_directory, length + 1);
    *out = store;
    return CN_PROGRESS_OK;
}

void cn_progress_store_close(cn_progress_store *store)
{
    if (!store)
        return;
    free(store->directory);
    free(store);
}

cn_progress_result cn_progress_store_load(cn_progress_store *store,
                                          const cn_book_identity *identity,
                                          cn_progress_record *record)
{
    const char *token = cn_book_identity_token(identity);
    cn_progress_record loaded;
    struct stat st;
    unsigned char *data = NULL;
    char *path = NULL;
    size_t token_length;
    size_t expected;
    uint32_t identity_length;
    uint32_t location_length;
    uint32_t progress;
    uint32_t stored_crc;
    int fd = -1;
    cn_progress_result result = CN_PROGRESS_CORRUPT;

    if (!store || !token || !record)
        return CN_PROGRESS_INVALID;
    path = record_path(store, identity);
    if (!path)
        return CN_PROGRESS_NO_MEMORY;
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        result = errno == ENOENT ? CN_PROGRESS_MISSING : CN_PROGRESS_IO_ERROR;
        goto done;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        result = CN_PROGRESS_IO_ERROR;
        goto done;
    }
    if (st.st_size < 12 ||
        (uint64_t)st.st_size > (uint64_t)PROGRESS_HEADER_BYTES +
                               CN_BOOK_IDENTITY_TOKEN_BYTES +
                               CN_READER_POSITION_MAX_BYTES +
                               PROGRESS_CRC_BYTES) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    data = (unsigned char *)malloc((size_t)st.st_size);
    if (!data) {
        result = CN_PROGRESS_NO_MEMORY;
        goto done;
    }
    if (read_all(fd, data, (size_t)st.st_size) != 0) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    if (close(fd) != 0) {
        fd = -1;
        result = CN_PROGRESS_IO_ERROR;
        goto done;
    }
    fd = -1;
    if (memcmp(data, PROGRESS_MAGIC, sizeof PROGRESS_MAGIC) != 0) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    if (read_u32be(data + 8) != CN_PROGRESS_FORMAT_VERSION) {
        result = CN_PROGRESS_UNSUPPORTED;
        goto done;
    }
    if (st.st_size < PROGRESS_HEADER_BYTES + PROGRESS_CRC_BYTES) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    identity_length = read_u32be(data + 12);
    location_length = read_u32be(data + 16);
    progress = read_u32be(data + 20);
    if (identity_length == 0 ||
        identity_length >= CN_BOOK_IDENTITY_TOKEN_BYTES ||
        location_length == 0 ||
        location_length > CN_READER_POSITION_MAX_BYTES) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    expected = PROGRESS_HEADER_BYTES + (size_t)identity_length +
               (size_t)location_length + PROGRESS_CRC_BYTES;
    if (expected != (size_t)st.st_size) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    stored_crc = read_u32be(data + expected - PROGRESS_CRC_BYTES);
    if (stored_crc != progress_crc32(data, expected - PROGRESS_CRC_BYTES)) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    token_length = strlen(token);
    if (identity_length != token_length ||
        memcmp(data + PROGRESS_HEADER_BYTES, token, token_length) != 0) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    if (progress != UINT32_MAX && progress > 10000) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }
    if (data[PROGRESS_HEADER_BYTES + identity_length] != '/' ||
        utf8_valid(data + PROGRESS_HEADER_BYTES + identity_length,
                   location_length) != 0) {
        result = CN_PROGRESS_CORRUPT;
        goto done;
    }

    cn_progress_record_init(&loaded);
    loaded.position.location = (char *)malloc((size_t)location_length + 1);
    if (!loaded.position.location) {
        result = CN_PROGRESS_NO_MEMORY;
        goto done;
    }
    memcpy(loaded.position.location,
           data + PROGRESS_HEADER_BYTES + identity_length, location_length);
    loaded.position.location[location_length] = '\0';
    loaded.position.progress_10000 =
        progress == UINT32_MAX ? -1 : (int)progress;
    cn_progress_record_clear(record);
    *record = loaded;
    result = CN_PROGRESS_OK;

done:
    if (fd >= 0)
        (void)close(fd);
    free(data);
    free(path);
    return result;
}

cn_progress_result cn_progress_store_save(cn_progress_store *store,
                                          const cn_book_identity *identity,
                                          const cn_progress_record *record)
{
    const char *token = cn_book_identity_token(identity);
    const char *location;
    unsigned char *data = NULL;
    char *path = NULL;
    char *temporary = NULL;
    size_t token_length;
    size_t location_length;
    size_t total;
    size_t temporary_length;
    int fd = -1;
    int directory_fd;
    cn_progress_result result = CN_PROGRESS_IO_ERROR;

    if (!store || !token || !record)
        return CN_PROGRESS_INVALID;
    location = record->position.location;
    if (bounded_length(location, CN_READER_POSITION_MAX_BYTES,
                       &location_length) != 0 || location[0] != '/' ||
        utf8_valid((const unsigned char *)location, location_length) != 0 ||
        record->position.progress_10000 < -1 ||
        record->position.progress_10000 > 10000)
        return CN_PROGRESS_INVALID;

    token_length = strlen(token);
    total = PROGRESS_HEADER_BYTES + token_length + location_length +
            PROGRESS_CRC_BYTES;
    data = (unsigned char *)malloc(total);
    path = record_path(store, identity);
    if (!data || !path) {
        result = CN_PROGRESS_NO_MEMORY;
        goto done;
    }
    memcpy(data, PROGRESS_MAGIC, sizeof PROGRESS_MAGIC);
    write_u32be(data + 8, CN_PROGRESS_FORMAT_VERSION);
    write_u32be(data + 12, (uint32_t)token_length);
    write_u32be(data + 16, (uint32_t)location_length);
    write_u32be(data + 20, record->position.progress_10000 < 0
                             ? UINT32_MAX
                             : (uint32_t)record->position.progress_10000);
    memcpy(data + PROGRESS_HEADER_BYTES, token, token_length);
    memcpy(data + PROGRESS_HEADER_BYTES + token_length,
           location, location_length);
    write_u32be(data + total - PROGRESS_CRC_BYTES,
                progress_crc32(data, total - PROGRESS_CRC_BYTES));

    temporary_length = strlen(path) + 32;
    temporary = (char *)malloc(temporary_length);
    if (!temporary) {
        result = CN_PROGRESS_NO_MEMORY;
        goto done;
    }
    snprintf(temporary, temporary_length, "%s.tmp.%ld", path, (long)getpid());
    if (unlink(temporary) != 0 && errno != ENOENT)
        goto done;
    fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        goto done;
    if (write_all(fd, data, total) != 0 || fsync(fd) != 0)
        goto done;
    if (close(fd) != 0) {
        fd = -1;
        goto done;
    }
    fd = -1;
    if (rename(temporary, path) != 0)
        goto done;

    /* Best-effort directory synchronization. The complete record has already
     * replaced the old one atomically even on filesystems that reject this. */
    directory_fd = open(store->directory, O_RDONLY);
    if (directory_fd >= 0) {
        (void)fsync(directory_fd);
        (void)close(directory_fd);
    }
    result = CN_PROGRESS_OK;

done:
    if (fd >= 0)
        (void)close(fd);
    if (result != CN_PROGRESS_OK && temporary)
        (void)unlink(temporary);
    free(temporary);
    free(path);
    free(data);
    return result;
}

const char *cn_progress_result_name(cn_progress_result result)
{
    switch (result) {
    case CN_PROGRESS_OK:          return "ok";
    case CN_PROGRESS_MISSING:     return "missing";
    case CN_PROGRESS_CORRUPT:     return "corrupt";
    case CN_PROGRESS_UNSUPPORTED: return "unsupported";
    case CN_PROGRESS_IO_ERROR:    return "io-error";
    case CN_PROGRESS_INVALID:     return "invalid";
    case CN_PROGRESS_NO_MEMORY:   return "no-memory";
    default:                      return "unknown";
    }
}
