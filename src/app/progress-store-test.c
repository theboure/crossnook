/*
 * progress-store-test.c - local progress persistence host/device validation.
 *
 * The --save and --restore modes are intentionally separate invocations so
 * the build script proves that records survive process lifetime.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "progress/book_identity.h"
#include "progress/progress_store.h"
#include "reader/reader.h"

#define FORMAT_HEADER_BYTES 24
#define FORMAT_CRC_BYTES 4
#define PAGE_BYTES ((size_t)CN_READER_W * CN_READER_H * 2)

static void check(int condition, const char *name, int *failures)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        (*failures)++;
}

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

static uint32_t crc32_bytes(const unsigned char *data, size_t length)
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

static char *record_path(const char *state_directory,
                         const cn_book_identity *identity)
{
    const char *token = cn_book_identity_token(identity);
    size_t length;
    char *path;
    if (!token)
        return NULL;
    length = strlen(state_directory) + strlen(token) +
             sizeof CN_PROGRESS_FILE_SUFFIX + 2;
    path = (char *)malloc(length);
    if (path)
        snprintf(path, length, "%s/%s%s", state_directory, token,
                 CN_PROGRESS_FILE_SUFFIX);
    return path;
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

static int replace_file(const char *path, const unsigned char *data,
                        size_t length)
{
    int fd = open(path, O_WRONLY | O_TRUNC);
    int result;
    if (fd < 0)
        return -1;
    result = write_all(fd, data, length);
    if (close(fd) != 0)
        result = -1;
    return result;
}

static unsigned char *read_file(const char *path, size_t *length)
{
    struct stat st;
    unsigned char *data;
    size_t done = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    data = (unsigned char *)malloc((size_t)st.st_size);
    if (!data) {
        close(fd);
        return NULL;
    }
    while (done < (size_t)st.st_size) {
        ssize_t count = read(fd, data + done, (size_t)st.st_size - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            free(data);
            close(fd);
            return NULL;
        }
        done += (size_t)count;
    }
    close(fd);
    *length = done;
    return data;
}

static int set_record(cn_progress_record *record, const char *location,
                      int progress)
{
    size_t length = strlen(location);
    char *copy = (char *)malloc(length + 1);
    if (!copy)
        return -1;
    memcpy(copy, location, length + 1);
    cn_progress_record_clear(record);
    record->position.location = copy;
    record->position.progress_10000 = progress;
    return 0;
}

static int same_position(const cn_reader_position *a,
                         const cn_reader_position *b)
{
    return a->location && b->location &&
           strcmp(a->location, b->location) == 0 &&
           a->progress_10000 == b->progress_10000;
}

static int capture_page(cn_reader *reader, int page,
                        cn_progress_record *record)
{
    if (cn_reader_go(reader, page) != page)
        return -1;
    return cn_reader_get_position(reader, &record->position);
}

static int run_errors(const char *font, const char *book,
                      const char *state_directory)
{
    static const char LOCATION_A[] =
        "/body/DocFragment[1]/body/p[1]/text().0";
    static const char LOCATION_B[] =
        "/body/DocFragment[1]/body/p[2]/text().0";
    cn_progress_store *store = NULL;
    cn_progress_store *unwritable = NULL;
    cn_progress_record source;
    cn_progress_record loaded;
    cn_book_identity identity;
    cn_progress_result result;
    unsigned char *bytes = NULL;
    char *path = NULL;
    char *oversized = NULL;
    size_t length = 0;
    uint32_t identity_length;
    cn_reader_config config;
    cn_reader *reader = NULL;
    unsigned char *frame = NULL;
    int failures = 0;

    cn_progress_record_init(&source);
    cn_progress_record_init(&loaded);
    cn_book_identity_init(&identity);
    check(cn_book_identity_from_path(&identity, "books/error.epub") == 0,
          "path identity created", &failures);
    check(cn_progress_store_open(&store, state_directory) == CN_PROGRESS_OK,
          "explicit state directory opened", &failures);
    if (!store)
        return 1;
    path = record_path(state_directory, &identity);
    check(path != NULL, "safe record path created", &failures);

    result = cn_progress_store_load(store, &identity, &loaded);
    check(result == CN_PROGRESS_MISSING, "no state file reports missing",
          &failures);

    check(set_record(&source, LOCATION_A, 3210) == 0,
          "valid source record created", &failures);
    check(cn_progress_store_save(store, &identity, &source) == CN_PROGRESS_OK,
          "valid state saved", &failures);
    check(cn_progress_store_load(store, &identity, &loaded) == CN_PROGRESS_OK &&
          same_position(&source.position, &loaded.position),
          "valid state loaded exactly", &failures);

    check(set_record(&source, LOCATION_B, 7777) == 0 &&
          cn_progress_store_save(store, &identity, &source) == CN_PROGRESS_OK &&
          cn_progress_store_load(store, &identity, &loaded) == CN_PROGRESS_OK &&
          same_position(&source.position, &loaded.position),
          "atomic overwrite replaces an existing record", &failures);

    check(cn_progress_store_save(store, &identity, &source) == CN_PROGRESS_OK,
          "record reset before truncation probe", &failures);
    bytes = read_file(path, &length);
    check(bytes && length > 12 && replace_file(path, bytes, 10) == 0 &&
          cn_progress_store_load(store, &identity, &loaded) ==
              CN_PROGRESS_CORRUPT,
          "truncated state rejected", &failures);
    free(bytes);
    bytes = NULL;

    check(replace_file(path, (const unsigned char *)"malformed", 9) == 0 &&
          cn_progress_store_load(store, &identity, &loaded) ==
              CN_PROGRESS_CORRUPT,
          "malformed state rejected", &failures);

    check(cn_progress_store_save(store, &identity, &source) == CN_PROGRESS_OK,
          "record reset before version probe", &failures);
    bytes = read_file(path, &length);
    if (bytes && length >= FORMAT_HEADER_BYTES + FORMAT_CRC_BYTES) {
        write_u32be(bytes + 8, CN_PROGRESS_FORMAT_VERSION + 1);
        write_u32be(bytes + length - FORMAT_CRC_BYTES,
                    crc32_bytes(bytes, length - FORMAT_CRC_BYTES));
    }
    check(bytes && replace_file(path, bytes, length) == 0 &&
          cn_progress_store_load(store, &identity, &loaded) ==
              CN_PROGRESS_UNSUPPORTED,
          "unknown format version rejected", &failures);
    free(bytes);
    bytes = NULL;

    check(cn_progress_store_save(store, &identity, &source) == CN_PROGRESS_OK,
          "record reset before UTF-8 probe", &failures);
    bytes = read_file(path, &length);
    if (bytes && length >= FORMAT_HEADER_BYTES + FORMAT_CRC_BYTES) {
        identity_length = read_u32be(bytes + 12);
        if ((size_t)FORMAT_HEADER_BYTES + identity_length + 1 < length)
            bytes[FORMAT_HEADER_BYTES + identity_length + 1] = 0xc0;
        write_u32be(bytes + length - FORMAT_CRC_BYTES,
                    crc32_bytes(bytes, length - FORMAT_CRC_BYTES));
    }
    check(bytes && replace_file(path, bytes, length) == 0 &&
          cn_progress_store_load(store, &identity, &loaded) ==
              CN_PROGRESS_CORRUPT,
          "persisted invalid UTF-8 rejected", &failures);
    free(bytes);
    bytes = NULL;

    check(cn_progress_store_save(store, &identity, &source) == CN_PROGRESS_OK,
          "record reset before invalid-save probes", &failures);
    oversized = (char *)malloc(CN_READER_POSITION_MAX_BYTES + 2);
    if (oversized) {
        memset(oversized, 'a', CN_READER_POSITION_MAX_BYTES + 1);
        oversized[0] = '/';
        oversized[CN_READER_POSITION_MAX_BYTES + 1] = '\0';
        free(source.position.location);
        source.position.location = oversized;
        source.position.progress_10000 = 10;
    }
    check(oversized &&
          cn_progress_store_save(store, &identity, &source) ==
              CN_PROGRESS_INVALID,
          "oversized position rejected before write", &failures);
    source.position.location = NULL;
    free(oversized);
    oversized = NULL;
    check(cn_progress_store_load(store, &identity, &loaded) == CN_PROGRESS_OK &&
          loaded.position.progress_10000 == 7777,
          "failed oversized save preserves last good record", &failures);

    check(set_record(&source, LOCATION_A, 10001) == 0 &&
          cn_progress_store_save(store, &identity, &source) ==
              CN_PROGRESS_INVALID,
          "out-of-range progress rejected", &failures);
    {
        char invalid_utf8[] = { '/', (char)0xc0, (char)0xaf, '\0' };
        check(set_record(&source, invalid_utf8, 1) == 0 &&
              cn_progress_store_save(store, &identity, &source) ==
                  CN_PROGRESS_INVALID,
              "invalid UTF-8 save rejected", &failures);
    }

    memset(&config, 0, sizeof config);
    config.font_path = font;
    reader = cn_reader_new(&config);
    frame = (unsigned char *)malloc(PAGE_BYTES);
    check(reader && frame && cn_reader_open(reader, book) == 0,
          "reader opened for corrupt-state fallback", &failures);
    check(replace_file(path, (const unsigned char *)"malformed", 9) == 0,
          "corrupt fallback record installed", &failures);
    result = cn_progress_store_load(store, &identity, &loaded);
    if (result == CN_PROGRESS_OK)
        (void)cn_reader_goto_position(reader, &loaded.position);
    check(result == CN_PROGRESS_CORRUPT && cn_reader_page(reader) == 0 &&
          frame && cn_reader_render(reader, frame,
                                    CN_READER_W, CN_READER_H) == 0,
          "corrupt state leaves Reader usable at page 0", &failures);

    result = cn_progress_store_open(&unwritable, "/proc");
    check(result == CN_PROGRESS_OK &&
          cn_progress_store_save(unwritable, &identity, &loaded) ==
              CN_PROGRESS_IO_ERROR,
          "unwritable state directory fails safely", &failures);

    free(frame);
    cn_reader_free(reader);
    cn_progress_store_close(unwritable);
    cn_progress_store_close(store);
    cn_progress_record_clear(&loaded);
    cn_progress_record_clear(&source);
    free(path);
    printf("PROGRESS ERROR SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_save(const char *font, const char *book_a, const char *book_b,
                    const char *foreign, const char *state_directory)
{
    cn_reader_config config;
    cn_reader *reader;
    cn_progress_store *store = NULL;
    cn_progress_record record_a;
    cn_progress_record record_b;
    cn_progress_record probe;
    cn_book_identity identity_a;
    cn_book_identity identity_b;
    cn_book_identity identity_foreign;
    int pages_a = 0;
    int page_a = 0;
    int page_b = 0;
    int failures = 0;

    (void)foreign;
    memset(&config, 0, sizeof config);
    config.font_path = font;
    cn_progress_record_init(&record_a);
    cn_progress_record_init(&record_b);
    cn_progress_record_init(&probe);
    cn_book_identity_init(&identity_a);
    cn_book_identity_init(&identity_b);
    cn_book_identity_init(&identity_foreign);

    check(cn_book_identity_from_path(&identity_a, book_a) == 0 &&
          cn_book_identity_from_path(&identity_b, book_b) == 0 &&
          cn_book_identity_from_path(&identity_foreign, foreign) == 0,
          "three independent path identities created", &failures);
    check(strcmp(cn_book_identity_token(&identity_a),
                 cn_book_identity_token(&identity_b)) != 0,
          "book identities are independent", &failures);
    check(cn_progress_store_open(&store, state_directory) == CN_PROGRESS_OK,
          "writer opened explicit state directory", &failures);
    reader = cn_reader_new(&config);
    check(reader != NULL, "writer reader created", &failures);
    if (!store || !reader)
        return 1;

    check(cn_reader_open(reader, book_a) == 0, "writer opened book A",
          &failures);
    pages_a = cn_reader_pages(reader);
    page_a = pages_a / 3;
    check(capture_page(reader, page_a, &record_a) == 0,
          "writer captured book A logical position", &failures);
    check(cn_progress_store_save(store, &identity_a, &record_a) ==
              CN_PROGRESS_OK,
          "writer saved book A", &failures);
    check(cn_progress_store_save(store, &identity_foreign, &record_a) ==
              CN_PROGRESS_OK,
          "writer saved mismatch probe through disk", &failures);

    check(cn_reader_open(reader, book_b) == 0, "writer opened book B",
          &failures);
    check(capture_page(reader, cn_reader_pages(reader) / 4, &record_b) == 0 &&
          cn_progress_store_save(store, &identity_b, &record_b) ==
              CN_PROGRESS_OK,
          "writer created initial book B record", &failures);
    page_b = cn_reader_pages(reader) / 2;
    check(capture_page(reader, page_b, &record_b) == 0 &&
          cn_progress_store_save(store, &identity_b, &record_b) ==
              CN_PROGRESS_OK,
          "writer atomically overwrote book B", &failures);
    check(cn_progress_store_load(store, &identity_a, &probe) == CN_PROGRESS_OK &&
          same_position(&probe.position, &record_a.position),
          "saving book B did not alter book A", &failures);

    printf("PROGRESS process=A pages=%d pageA=%d pageB=%d progress=%d\n",
           pages_a, page_a, page_b, record_a.position.progress_10000);
    cn_reader_free(reader);
    cn_progress_store_close(store);
    cn_progress_record_clear(&probe);
    cn_progress_record_clear(&record_b);
    cn_progress_record_clear(&record_a);
    printf("PROGRESS SAVE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int restore_one(cn_reader *reader, cn_progress_store *store,
                       const cn_book_identity *identity, const char *book,
                       int expected_pages, int expected_page,
                       int verify_first_visible,
                       cn_progress_record *loaded, int *failures)
{
    cn_reader_position recaptured;
    int ok;
    cn_reader_position_init(&recaptured);
    ok = cn_reader_open(reader, book) == 0 &&
         cn_progress_store_load(store, identity, loaded) == CN_PROGRESS_OK &&
         cn_reader_goto_position(reader, &loaded->position) == 0 &&
         cn_reader_pages(reader) == expected_pages &&
         cn_reader_page(reader) == expected_page;
    if (ok && verify_first_visible)
        ok = cn_reader_get_position(reader, &recaptured) == 0 &&
             recaptured.location && loaded->position.location &&
             strcmp(recaptured.location, loaded->position.location) == 0;
    check(ok, "disk position restored exactly", failures);
    cn_reader_position_clear(&recaptured);
    return ok ? 0 : -1;
}

static int run_restore(const char *font, const char *book_a,
                       const char *book_b, const char *foreign,
                       const char *state_directory)
{
    cn_reader_config config_a;
    cn_reader_config config_b;
    cn_reader *reader_a;
    cn_reader *reader_b;
    cn_reader *reader_foreign;
    cn_progress_store *store = NULL;
    cn_progress_record loaded_a;
    cn_progress_record loaded_b;
    cn_progress_record loaded_foreign;
    cn_book_identity identity_a;
    cn_book_identity identity_b;
    cn_book_identity identity_foreign;
    unsigned char *frame = NULL;
    int failures = 0;

    memset(&config_a, 0, sizeof config_a);
    config_a.font_path = font;
    memset(&config_b, 0, sizeof config_b);
    config_b.font_path = font;
    config_b.font_size = 40;
    config_b.margin_px = 24;
    cn_progress_record_init(&loaded_a);
    cn_progress_record_init(&loaded_b);
    cn_progress_record_init(&loaded_foreign);
    cn_book_identity_from_path(&identity_a, book_a);
    cn_book_identity_from_path(&identity_b, book_b);
    cn_book_identity_from_path(&identity_foreign, foreign);

    check(cn_progress_store_open(&store, state_directory) == CN_PROGRESS_OK,
          "process B opened process A state", &failures);
    reader_a = cn_reader_new(&config_b);
    reader_b = cn_reader_new(&config_a);
    reader_foreign = cn_reader_new(&config_a);
    check(reader_a && reader_b && reader_foreign,
          "process B readers created", &failures);
    if (!store || !reader_a || !reader_b || !reader_foreign)
        return 1;

    restore_one(reader_a, store, &identity_a, book_a, 254, 82, 0,
                &loaded_a, &failures);
    check(loaded_a.position.progress_10000 == 3210,
          "normalized metadata survived process restart", &failures);
    printf("PROGRESS relayout pages=79->%d page=26->%d progress=%d\n",
           cn_reader_pages(reader_a), cn_reader_page(reader_a),
           loaded_a.position.progress_10000);

    restore_one(reader_b, store, &identity_b, book_b, 79, 39, 1,
                &loaded_b, &failures);
    check(loaded_a.position.location && loaded_b.position.location &&
          strcmp(loaded_a.position.location, loaded_b.position.location) != 0,
          "multiple books retain independent logical positions", &failures);

    check(cn_reader_open(reader_foreign, foreign) == 0 &&
          cn_progress_store_load(store, &identity_foreign, &loaded_foreign) ==
              CN_PROGRESS_OK &&
          cn_reader_goto_position(reader_foreign, &loaded_foreign.position) ==
              -1 &&
          cn_reader_page(reader_foreign) == 0,
          "persisted position invalid for document is rejected at page 0",
          &failures);
    frame = (unsigned char *)malloc(PAGE_BYTES);
    check(frame && cn_reader_render(reader_foreign, frame,
                                    CN_READER_W, CN_READER_H) == 0,
          "reader remains renderable after rejected restore", &failures);

    free(frame);
    cn_reader_free(reader_foreign);
    cn_reader_free(reader_b);
    cn_reader_free(reader_a);
    cn_progress_store_close(store);
    cn_progress_record_clear(&loaded_foreign);
    cn_progress_record_clear(&loaded_b);
    cn_progress_record_clear(&loaded_a);
    printf("PROGRESS PROCESS RESTART failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--errors") == 0)
        return run_errors(argv[2], argv[3], argv[4]);
    if (argc == 7 && strcmp(argv[1], "--save") == 0)
        return run_save(argv[2], argv[3], argv[4], argv[5], argv[6]);
    if (argc == 7 && strcmp(argv[1], "--restore") == 0)
        return run_restore(argv[2], argv[3], argv[4], argv[5], argv[6]);
    fprintf(stderr,
            "usage: crossnook-progress-test --errors "
            "<font> <book> <state-dir>\n"
            "       crossnook-progress-test --save|--restore "
            "<font> <book-a> <book-b> <foreign-book> <state-dir>\n");
    return 2;
}
