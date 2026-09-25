/* Deterministic host matrix and verified-card device identity diagnostic. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "identity/device_identity.h"
#include "platform/storage_verify.h"

static int failures;

static void check(int condition, const char *name)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        ++failures;
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

static int write_all_file(const char *path, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    ssize_t written;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return 0;
    while (done < length) {
        written = write(fd, bytes + done, length - done);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0) {
            (void)close(fd);
            return 0;
        }
        done += (size_t)written;
    }
    return close(fd) == 0;
}

static int read_file(const char *path, unsigned char *output, size_t capacity,
                     size_t *length)
{
    size_t done = 0;
    ssize_t got;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    while (done < capacity) {
        got = read(fd, output + done, capacity - done);
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            (void)close(fd);
            return 0;
        }
        if (got == 0) {
            if (close(fd) != 0)
                return 0;
            *length = done;
            return 1;
        }
        done += (size_t)got;
    }
    (void)close(fd);
    return 0;
}

static int fixture(const char *path, const char *id, int bad_crc,
                   const char *version)
{
    char data[CN_DEVICE_ID_FILE_MAX_BYTES];
    size_t prefix_length;
    int written;
    uint32_t checksum;
    written = snprintf(data, sizeof data, "crossnook-device-identity=%s\n"
                       "id=%s\n", version, id);
    if (written < 0 || (size_t)written + sizeof "crc32=" + 8 >= sizeof data)
        return 0;
    prefix_length = (size_t)written;
    checksum = crc32((const unsigned char *)data, prefix_length);
    if (bad_crc)
        checksum ^= 1u;
    written = snprintf(data + prefix_length, sizeof data - prefix_length,
                       "crc32=%08x\n", checksum);
    return written > 0 && write_all_file(path, data,
                                         prefix_length + (size_t)written);
}

typedef struct entropy_fixture {
    unsigned char bytes[CN_DEVICE_ID_BYTES];
    int fail;
    int calls;
    const char *winner_path;
} entropy_fixture;

static void encode_id(const unsigned char bytes[CN_DEVICE_ID_BYTES],
                      char output[CN_DEVICE_ID_TEXT_CAPACITY])
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < CN_DEVICE_ID_BYTES; ++i) {
        output[i * 2] = hex[bytes[i] >> 4];
        output[i * 2 + 1] = hex[bytes[i] & 15];
    }
    output[CN_DEVICE_ID_TEXT_LENGTH] = '\0';
}

static int injected_entropy(void *context, unsigned char *output, size_t length)
{
    entropy_fixture *fixture_data = (entropy_fixture *)context;
    char id[CN_DEVICE_ID_TEXT_CAPACITY];
    ++fixture_data->calls;
    if (fixture_data->fail || length != CN_DEVICE_ID_BYTES)
        return -1;
    if (fixture_data->winner_path) {
        encode_id(fixture_data->bytes, id);
        if (!fixture(fixture_data->winner_path, id, 0, "1"))
            return -1;
    }
    memcpy(output, fixture_data->bytes, length);
    return 0;
}

static int prepare_root(const char *root, cn_storage_layout *layout)
{
    return cn_storage_layout_init(layout, root, NULL) == CN_STORAGE_OK &&
           cn_storage_layout_prepare(layout, NULL) == CN_STORAGE_OK;
}

static int init_store(const char *root, cn_storage_layout *layout,
                      cn_device_identity_store *store,
                      cn_device_identity_entropy_fn entropy, void *context)
{
    return prepare_root(root, layout) &&
           cn_device_identity_store_init(store, layout, entropy, context) ==
               CN_DEVICE_ID_OK;
}

static int identity_path(const cn_storage_layout *layout, char *path,
                         size_t capacity)
{
    char config[CN_STORAGE_PATH_CAPACITY];
    int written;
    if (cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG,
                               config, sizeof config) != CN_STORAGE_OK)
        return 0;
    written = snprintf(path, capacity, "%s/device-id", config);
    return written >= 0 && (size_t)written < capacity;
}

static int append_path(char *output, size_t capacity, const char *base,
                       const char *suffix)
{
    size_t base_length = strlen(base);
    size_t suffix_length = strlen(suffix);
    if (base_length + suffix_length + 1 > capacity)
        return 0;
    memcpy(output, base, base_length);
    memcpy(output + base_length, suffix, suffix_length + 1);
    return 1;
}

static int smoke(void)
{
    char root_template[] = "/tmp/crossnook-device-identity.XXXXXX";
    char root_one[CN_STORAGE_PATH_CAPACITY];
    char root_two[CN_STORAGE_PATH_CAPACITY];
    char root_three[CN_STORAGE_PATH_CAPACITY];
    char root_four[CN_STORAGE_PATH_CAPACITY];
    char path[CN_STORAGE_PATH_CAPACITY];
    char first[CN_DEVICE_ID_TEXT_CAPACITY];
    char second[CN_DEVICE_ID_TEXT_CAPACITY];
    char small[CN_DEVICE_ID_TEXT_CAPACITY];
    unsigned char before[CN_DEVICE_ID_FILE_MAX_BYTES];
    unsigned char after[CN_DEVICE_ID_FILE_MAX_BYTES];
    size_t before_length, after_length;
    struct stat before_stat, after_stat;
    cn_storage_layout layout_one, layout_two, layout_three, layout_four;
    cn_device_identity_store store_one, store_two, store_three, store_four;
    entropy_fixture entropy_one, entropy_two, entropy_fail, entropy_winner;
    char *created;
    int before_calls;

    created = mkdtemp(root_template);
    if (!created)
        return 1;
    snprintf(root_one, sizeof root_one, "%s/one", created);
    snprintf(root_two, sizeof root_two, "%s/two", created);
    snprintf(root_three, sizeof root_three, "%s/three", created);
    snprintf(root_four, sizeof root_four, "%s/four", created);
    check(mkdir(root_one, 0700) == 0 && mkdir(root_two, 0700) == 0 &&
          mkdir(root_three, 0700) == 0 && mkdir(root_four, 0700) == 0,
          "four existing caller roots created");
    memset(&entropy_one, 0, sizeof entropy_one);
    memset(&entropy_two, 0, sizeof entropy_two);
    memset(&entropy_fail, 0, sizeof entropy_fail);
    memset(&entropy_winner, 0, sizeof entropy_winner);
    memset(entropy_one.bytes, 0x11, sizeof entropy_one.bytes);
    memset(entropy_two.bytes, 0x22, sizeof entropy_two.bytes);
    memset(entropy_fail.bytes, 0x33, sizeof entropy_fail.bytes);
    memset(entropy_winner.bytes, 0x44, sizeof entropy_winner.bytes);
    entropy_fail.fail = 1;

    check(init_store(root_one, &layout_one, &store_one, injected_entropy,
                     &entropy_one), "prepared root initializes identity store");
    check(identity_path(&layout_one, path, sizeof path), "identity path derived");
    check(cn_device_identity_load_or_create(&store_one, first, sizeof first,
                                            NULL) == CN_DEVICE_ID_CREATED &&
          strlen(first) == CN_DEVICE_ID_TEXT_LENGTH,
          "first creation returns CREATED and 32-character ID");
    check(read_file(path, before, sizeof before, &before_length) &&
          stat(path, &before_stat) == 0, "created identity file captured");
    before_calls = entropy_one.calls;
    check(cn_device_identity_load_or_create(&store_one, second, sizeof second,
                                            NULL) == CN_DEVICE_ID_OK &&
          !strcmp(first, second) && entropy_one.calls == before_calls,
          "reopen returns identical ID without entropy");
    check(read_file(path, after, sizeof after, &after_length) &&
          stat(path, &after_stat) == 0 && after_length == before_length &&
          !memcmp(before, after, before_length) &&
          before_stat.st_ino == after_stat.st_ino,
          "valid identity is not rewritten");

    check(init_store(root_two, &layout_two, &store_two, injected_entropy,
                     &entropy_two) &&
          cn_device_identity_load_or_create(&store_two, second, sizeof second,
                                            NULL) == CN_DEVICE_ID_CREATED &&
          strcmp(first, second) != 0,
          "two roots with different injected entropy differ");

    check(write_all_file(path, "bad", 3) &&
          cn_device_identity_load_or_create(&store_one, small, sizeof small,
                                            NULL) == CN_DEVICE_ID_MALFORMED &&
          small[0] == '\0', "malformed file is a hard failure");
    check(read_file(path, after, sizeof after, &after_length) && after_length == 3,
          "malformed file is not replaced");
    small[0] = 'x';
    check(fixture(path, first, 0, "2") &&
          cn_device_identity_load_or_create(&store_one, small, sizeof small,
                                            NULL) == CN_DEVICE_ID_UNSUPPORTED_VERSION &&
          small[0] == '\0', "unsupported version is rejected and cleared");
    small[0] = 'x';
    check(fixture(path, first, 1, "1") &&
          cn_device_identity_load_or_create(&store_one, small, sizeof small,
                                            NULL) == CN_DEVICE_ID_BAD_CHECKSUM &&
          small[0] == '\0', "bad checksum is rejected and cleared");

    check(init_store(root_three, &layout_three, &store_three, injected_entropy,
                     &entropy_fail) &&
          cn_device_identity_load_or_create(&store_three, small, sizeof small,
                                            NULL) == CN_DEVICE_ID_ENTROPY_FAILED,
          "entropy failure is explicit");
    check(cn_device_identity_load_or_create(&store_three, small,
                                            CN_DEVICE_ID_TEXT_LENGTH, NULL) ==
              CN_DEVICE_ID_BUFFER_TOO_SMALL && small[0] == '\0',
          "insufficient output capacity is rejected and cleared");
    strcpy(small, "not-cleared");
    check(cn_device_identity_load_or_create(NULL, small, sizeof small, NULL) ==
              CN_DEVICE_ID_INVALID_ARGUMENT && small[0] == '\0',
          "invalid store argument is rejected and cleared");
    check(cn_device_identity_load_or_create(&store_three, NULL, 0, NULL) ==
              CN_DEVICE_ID_INVALID_ARGUMENT,
          "null output argument is rejected");

    (void)append_path(path, sizeof path, root_three, "/config");
    (void)rmdir(path);
    check(cn_storage_layout_init(&layout_three, root_three, NULL) == CN_STORAGE_OK &&
          cn_device_identity_store_init(&store_three, &layout_three,
                                        injected_entropy, &entropy_fail) ==
              CN_DEVICE_ID_NOT_FOUND,
          "missing config directory is caller failure; module does not create it");

    check(init_store(root_four, &layout_four, &store_four, injected_entropy,
                     &entropy_winner) &&
          identity_path(&layout_four, path, sizeof path),
          "race fixture store initialized");
    entropy_winner.winner_path = store_four.path;
    check(cn_device_identity_load_or_create(&store_four, second, sizeof second,
                                            NULL) == CN_DEVICE_ID_OK &&
          entropy_winner.calls == 1 && strlen(second) == CN_DEVICE_ID_TEXT_LENGTH,
          "EEXIST reopens and validates the winning identity");

    (void)unlink(path);
    (void)unlink(store_three.path);
    (void)append_path(path, sizeof path, root_one, "/config/device-id");
    (void)unlink(path);
    (void)append_path(path, sizeof path, root_two, "/config/device-id");
    (void)unlink(path);
    (void)append_path(path, sizeof path, root_one, "/config");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_one, "/state/progress");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_one, "/state");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_two, "/config");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_two, "/state/progress");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_two, "/state");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_three, "/state/progress");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_three, "/state");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_four, "/config");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_four, "/state/progress");
    (void)rmdir(path);
    (void)append_path(path, sizeof path, root_four, "/state");
    (void)rmdir(path);
    (void)rmdir(root_one);
    (void)rmdir(root_two);
    (void)rmdir(root_three);
    (void)rmdir(root_four);
    (void)rmdir(created);
    printf("DEVICE IDENTITY SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int parse_number(const char *text, unsigned *output)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno || !text[0] || *end || value > 65535)
        return 0;
    *output = (unsigned)value;
    return 1;
}

static int physical(int argc, char **argv)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_storage_layout layout;
    cn_device_identity_store store;
    cn_device_identity_result result;
    unsigned major_number, minor_number;
    char identity[CN_DEVICE_ID_TEXT_CAPACITY];
    unsigned char before[CN_DEVICE_ID_FILE_MAX_BYTES];
    unsigned char after[CN_DEVICE_ID_FILE_MAX_BYTES];
    size_t before_length = 0, after_length = 0;
    int existed, unchanged = 0;
    struct stat before_stat, after_stat;
    (void)argc;

    candidate.mountpoint = argv[2];
    candidate.root = argv[3];
    if (!parse_number(argv[4], &major_number) ||
        !parse_number(argv[5], &minor_number))
        return 2;
    candidate.expected_major = major_number;
    candidate.expected_minor = minor_number;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) !=
        CN_PLATFORM_STORAGE_OK) {
        puts("DEVICE IDENTITY GATE storage=unverified persistence=not-attempted "
             "network=not-attempted");
        return 1;
    }
    if (cn_storage_layout_init(&layout, verified.root, NULL) != CN_STORAGE_OK ||
        cn_storage_layout_prepare(&layout, NULL) != CN_STORAGE_OK ||
        cn_device_identity_store_init(&store, &layout, NULL, NULL) !=
            CN_DEVICE_ID_OK)
        return 1;
    existed = stat(store.path, &before_stat) == 0 &&
              read_file(store.path, before, sizeof before, &before_length);
    result = cn_device_identity_load_or_create(&store, identity, sizeof identity,
                                               NULL);
    if (result == CN_DEVICE_ID_OK && existed) {
        unchanged = stat(store.path, &after_stat) == 0 &&
                    read_file(store.path, after, sizeof after, &after_length) &&
                    before_length == after_length &&
                    !memcmp(before, after, before_length) &&
                    before_stat.st_ino == after_stat.st_ino;
    }
    printf("DEVICE IDENTITY GATE storage=verified result=%s id-length=%zu "
           "id-check=%08x existing-unchanged=%s network=not-attempted\n",
           cn_device_identity_result_name(result), strlen(identity),
           crc32((const unsigned char *)identity, strlen(identity)),
           existed ? (unchanged ? "yes" : "no") : "not-applicable");
    return result == CN_DEVICE_ID_OK || result == CN_DEVICE_ID_CREATED ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--smoke") == 0)
        return smoke();
    if (argc == 6 && strcmp(argv[1], "--physical") == 0)
        return physical(argc, argv);
    fprintf(stderr, "usage: device-identity-test --smoke | --physical "
                    "<mount> <root> <major> <minor>\n");
    return 2;
}
