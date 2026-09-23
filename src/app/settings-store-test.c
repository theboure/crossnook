/* Persistent Settings Core host/device diagnostic. */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "settings/settings_store.h"
#include "storage/storage_layout.h"

typedef enum fault_mode {
    FAULT_NONE = 0,
    FAULT_WRITE_NOSPC,
    FAULT_WRITE_QUOTA,
    FAULT_TEMP_FSYNC,
    FAULT_TEMP_CLOSE,
    FAULT_RENAME,
    FAULT_DIRECTORY_FSYNC
} fault_mode;

static fault_mode active_fault;
static int wrapped_write_calls;
static int wrapped_fsync_calls;
static int wrapped_close_calls;
static int wrapped_rename_calls;

ssize_t __real_write(int fd, const void *data, size_t length);
int __real_fsync(int fd);
int __real_close(int fd);
int __real_rename(const char *old_path, const char *new_path);

ssize_t __wrap_write(int fd, const void *data, size_t length)
{
    if (active_fault != FAULT_NONE && ++wrapped_write_calls == 1 &&
        (active_fault == FAULT_WRITE_NOSPC ||
         active_fault == FAULT_WRITE_QUOTA)) {
        errno = active_fault == FAULT_WRITE_NOSPC ? ENOSPC : EDQUOT;
        return -1;
    }
    return __real_write(fd, data, length);
}

int __wrap_fsync(int fd)
{
    if (active_fault != FAULT_NONE) {
        ++wrapped_fsync_calls;
        if ((active_fault == FAULT_TEMP_FSYNC && wrapped_fsync_calls == 1) ||
            (active_fault == FAULT_DIRECTORY_FSYNC &&
             wrapped_fsync_calls == 2)) {
            errno = EIO;
            return -1;
        }
    }
    return __real_fsync(fd);
}

int __wrap_close(int fd)
{
    int result = __real_close(fd);
    if (active_fault != FAULT_NONE && ++wrapped_close_calls == 1 &&
        active_fault == FAULT_TEMP_CLOSE) {
        errno = EIO;
        return -1;
    }
    return result;
}

int __wrap_rename(const char *old_path, const char *new_path)
{
    if (active_fault != FAULT_NONE && ++wrapped_rename_calls == 1 &&
        active_fault == FAULT_RENAME) {
        errno = EIO;
        return -1;
    }
    return __real_rename(old_path, new_path);
}

static void set_fault(fault_mode mode)
{
    active_fault = mode;
    wrapped_write_calls = 0;
    wrapped_fsync_calls = 0;
    wrapped_close_calls = 0;
    wrapped_rename_calls = 0;
}

static void clear_fault(void)
{
    active_fault = FAULT_NONE;
}

static void check(int condition, const char *name, int *failures)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        ++*failures;
}

static int join_path(char *out, size_t capacity,
                     const char *parent, const char *child)
{
    int written = snprintf(out, capacity, "%s/%s", parent, child);
    return written >= 0 && (size_t)written < capacity ? 0 : -1;
}

static int write_bytes(const char *path, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return -1;
    while (done < length) {
        ssize_t count = write(fd, bytes + done, length - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0) {
            (void)close(fd);
            return -1;
        }
        done += (size_t)count;
    }
    return close(fd);
}

static int read_bytes(const char *path, unsigned char *data, size_t capacity,
                      size_t *length)
{
    size_t done = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    for (;;) {
        ssize_t count;
        if (done == capacity) {
            (void)close(fd);
            return -1;
        }
        count = read(fd, data + done, capacity - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0) {
            (void)close(fd);
            return -1;
        }
        if (count == 0)
            break;
        done += (size_t)count;
    }
    if (close(fd) != 0)
        return -1;
    *length = done;
    return 0;
}

static int file_equals(const char *path, const void *expected, size_t length)
{
    unsigned char data[CN_SETTINGS_FILE_MAX_BYTES + 1];
    size_t actual;
    return read_bytes(path, data, sizeof data, &actual) == 0 &&
           actual == length && memcmp(data, expected, length) == 0;
}

static int path_missing(const char *path)
{
    struct stat st;
    return lstat(path, &st) != 0 && errno == ENOENT;
}

static int file_mode_is(const char *path, mode_t mode)
{
    struct stat st;
    return lstat(path, &st) == 0 && (st.st_mode & 0777) == mode;
}

static int settings_equal(const cn_settings *left,
                          const cn_settings *right)
{
    return left->kosync_enabled == right->kosync_enabled &&
           strcmp(left->kosync_base_url, right->kosync_base_url) == 0 &&
           strcmp(left->kosync_device_name,
                  right->kosync_device_name) == 0;
}

static int is_defaults(const cn_settings *settings)
{
    cn_settings defaults;
    cn_settings_defaults(&defaults);
    return settings_equal(settings, &defaults);
}

static uint32_t test_crc32(const unsigned char *data, size_t length)
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

static int fixture_from_body(const unsigned char *body, size_t body_length,
                             unsigned char *file, size_t capacity,
                             size_t *file_length)
{
    static const char hex[] = "0123456789abcdef";
    char crc_line[] = "crc32=00000000\n";
    uint32_t crc = test_crc32(body, body_length);
    int shift;
    if (body_length + sizeof crc_line - 1 > capacity)
        return -1;
    memcpy(file, body, body_length);
    for (shift = 28; shift >= 0; shift -= 4)
        crc_line[6 + (28 - shift) / 4] = hex[(crc >> shift) & 0x0f];
    memcpy(file + body_length, crc_line, sizeof crc_line - 1);
    *file_length = body_length + sizeof crc_line - 1;
    return 0;
}

static int put_body(const char *path, const unsigned char *body,
                    size_t body_length)
{
    unsigned char file[CN_SETTINGS_FILE_MAX_BYTES];
    size_t file_length;
    return fixture_from_body(body, body_length, file, sizeof file,
                             &file_length) == 0 &&
           write_bytes(path, file, file_length) == 0 ? 0 : -1;
}

static int fixture_loads_as(cn_settings_store *store, const char *path,
                            const unsigned char *body, size_t body_length,
                            cn_settings_result expected)
{
    cn_settings loaded;
    int system_errno;
    if (put_body(path, body, body_length) != 0)
        return 0;
    memset(&loaded, 0xa5, sizeof loaded);
    return cn_settings_load(store, &loaded, &system_errno) == expected &&
           (expected == CN_SETTINGS_OK || is_defaults(&loaded));
}

static int compose_store(const char *root, cn_storage_layout *layout,
                         cn_settings_store *store,
                         char config[CN_STORAGE_PATH_CAPACITY])
{
    int system_errno;
    return cn_storage_layout_init(layout, root, &system_errno) ==
               CN_STORAGE_OK &&
           cn_storage_layout_prepare(layout, &system_errno) == CN_STORAGE_OK &&
           cn_storage_layout_path(layout, CN_STORAGE_LOCATION_CONFIG,
                                  config, CN_STORAGE_PATH_CAPACITY) ==
               CN_STORAGE_OK &&
           cn_settings_store_init(store, config, &system_errno) ==
               CN_SETTINGS_OK;
}

static void set_nondefault(cn_settings *settings)
{
    cn_settings_defaults(settings);
    settings->kosync_enabled = 1;
    memcpy(settings->kosync_base_url, "https://sync.example.test/base",
           sizeof "https://sync.example.test/base");
    memcpy(settings->kosync_device_name, "CrossNook Test",
           sizeof "CrossNook Test");
}

static int verify_crc_line(const unsigned char *data, size_t length)
{
    const unsigned char *line;
    uint32_t expected;
    uint32_t parsed = 0;
    size_t i;
    if (length < 15 || data[length - 1] != '\n')
        return 0;
    line = data + length - 15;
    if (memcmp(line, "crc32=", 6) != 0)
        return 0;
    for (i = 0; i < 8; ++i) {
        unsigned digit;
        if (line[6 + i] >= '0' && line[6 + i] <= '9')
            digit = (unsigned)(line[6 + i] - '0');
        else if (line[6 + i] >= 'a' && line[6 + i] <= 'f')
            digit = (unsigned)(line[6 + i] - 'a') + 10u;
        else
            return 0;
        parsed = (parsed << 4) | digit;
    }
    expected = test_crc32(data, length - 15);
    return parsed == expected;
}

static int serialized_keys_are_exact(const unsigned char *data, size_t length)
{
    static const char *const keys[] = {
        "crossnook-settings", "kosync.enabled", "kosync.base_url",
        "kosync.device_name", "crc32"
    };
    size_t offset = 0;
    size_t index;
    for (index = 0; index < sizeof keys / sizeof keys[0]; ++index) {
        size_t end = offset;
        size_t key_length = strlen(keys[index]);
        while (end < length && data[end] != '\n')
            ++end;
        if (end == length || end - offset < key_length + 1 ||
            memcmp(data + offset, keys[index], key_length) != 0 ||
            data[offset + key_length] != '=')
            return 0;
        offset = end + 1;
    }
    return offset == length;
}

static int fault_preserves_old(cn_settings_store *store,
                               const cn_settings *old_settings,
                               const cn_settings *new_settings,
                               fault_mode fault,
                               cn_settings_result expected,
                               int expected_errno)
{
    cn_settings loaded;
    char final_path[CN_SETTINGS_PATH_CAPACITY];
    char temporary[CN_SETTINGS_PATH_CAPACITY];
    int system_errno;
    cn_settings_result result;
    if (join_path(final_path, sizeof final_path, store->config_directory,
                  "settings.conf") != 0 ||
        snprintf(temporary, sizeof temporary, "%s.tmp.%ld", final_path,
                 (long)getpid()) >= (int)sizeof temporary)
        return 0;
    if (cn_settings_save(store, old_settings, &system_errno) != CN_SETTINGS_OK)
        return 0;
    set_fault(fault);
    result = cn_settings_save(store, new_settings, &system_errno);
    clear_fault();
    return result == expected && system_errno == expected_errno &&
           path_missing(temporary) &&
           cn_settings_load(store, &loaded, &system_errno) == CN_SETTINGS_OK &&
           settings_equal(&loaded, old_settings);
}

static int concurrent_visibility(cn_settings_store *store,
                                 const cn_settings *old_settings,
                                 const cn_settings *new_settings)
{
    int ready[2];
    pid_t child;
    int status;
    int system_errno;
    int i;
    char marker;
    if (cn_settings_save(store, old_settings, &system_errno) != CN_SETTINGS_OK)
        return 0;
    if (pipe(ready) != 0)
        return 0;
    child = fork();
    if (child < 0) {
        (void)close(ready[0]);
        (void)close(ready[1]);
        return 0;
    }
    if (child == 0) {
        int saw_old = 0;
        int saw_new = 0;
        (void)close(ready[0]);
        for (i = 0; i < 500; ++i) {
            cn_settings loaded;
            if (cn_settings_load(store, &loaded, &system_errno) !=
                    CN_SETTINGS_OK ||
                (!settings_equal(&loaded, old_settings) &&
                 !settings_equal(&loaded, new_settings)))
                _exit(1);
            if (settings_equal(&loaded, old_settings))
                saw_old = 1;
            if (settings_equal(&loaded, new_settings))
                saw_new = 1;
            if (i == 0 && write(ready[1], "r", 1) != 1)
                _exit(1);
            if (saw_old && saw_new)
                _exit(0);
            (void)usleep(1000);
        }
        _exit(1);
    }
    (void)close(ready[1]);
    if (read(ready[0], &marker, 1) != 1) {
        (void)close(ready[0]);
        (void)waitpid(child, &status, 0);
        return 0;
    }
    (void)close(ready[0]);
    for (i = 0; i < 40; ++i) {
        if (cn_settings_save(store, (i & 1) ? old_settings : new_settings,
                             &system_errno) != CN_SETTINGS_OK)
            break;
        (void)usleep(1000);
    }
    if (i == 40 &&
        cn_settings_save(store, new_settings, &system_errno) != CN_SETTINGS_OK)
        i = -1;
    if (waitpid(child, &status, 0) != child)
        return 0;
    return i == 40 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int run_smoke(const char *workspace)
{
    static const unsigned char unknown_body[] =
        "crossnook-settings=1\n"
        "kosync.enabled=false\n"
        "kosync.base_url=\n"
        "kosync.device_name=CrossNook\n"
        "unknown=value\n";
    static const unsigned char duplicate_body[] =
        "crossnook-settings=1\n"
        "kosync.enabled=false\n"
        "kosync.enabled=false\n"
        "kosync.base_url=\n"
        "kosync.device_name=CrossNook\n";
    static const unsigned char missing_body[] =
        "crossnook-settings=1\n"
        "kosync.enabled=false\n"
        "kosync.base_url=\n";
    static const unsigned char malformed_line_body[] =
        "crossnook-settings=1\n"
        "kosync.enabled:false\n"
        "kosync.base_url=\n"
        "kosync.device_name=CrossNook\n";
    static const unsigned char malformed_version_body[] =
        "crossnook-settings=01\n"
        "kosync.enabled=false\n"
        "kosync.base_url=\n"
        "kosync.device_name=CrossNook\n";
    static const unsigned char invalid_bool_body[] =
        "crossnook-settings=1\n"
        "kosync.enabled=yes\n"
        "kosync.base_url=\n"
        "kosync.device_name=CrossNook\n";
    static const unsigned char invalid_url_body[] =
        "crossnook-settings=1\n"
        "kosync.enabled=true\n"
        "kosync.base_url=http://sync.example.test\n"
        "kosync.device_name=CrossNook\n";
    static const unsigned char invalid_utf8_body[] =
        "crossnook-settings=1\n"
        "kosync.enabled=false\n"
        "kosync.base_url=\n"
        "kosync.device_name=bad\xc3\x28\n";
    static const unsigned char future_body[] =
        "crossnook-settings=2\n"
        "kosync.enabled=false\n"
        "kosync.base_url=\n"
        "kosync.device_name=CrossNook\n";
    static const unsigned char default_file[] =
        "crossnook-settings=1\n"
        "kosync.enabled=false\n"
        "kosync.base_url=\n"
        "kosync.device_name=CrossNook\n"
        "crc32=2a4c8551\n";
    static const unsigned char outside_text[] = "outside\n";
    cn_storage_layout layout;
    cn_settings_store store;
    cn_settings_store copied_store;
    cn_settings defaults;
    cn_settings old_settings;
    cn_settings new_settings;
    cn_settings loaded;
    cn_settings boundary;
    char root[CN_SETTINGS_PATH_CAPACITY];
    char config[CN_SETTINGS_PATH_CAPACITY];
    char mutable_config[CN_SETTINGS_PATH_CAPACITY];
    char settings_path[CN_SETTINGS_PATH_CAPACITY];
    char temp_path[CN_SETTINGS_PATH_CAPACITY];
    char outside[CN_SETTINGS_PATH_CAPACITY];
    char other[CN_SETTINGS_PATH_CAPACITY];
    unsigned char first[CN_SETTINGS_FILE_MAX_BYTES + 1];
    unsigned char second[CN_SETTINGS_FILE_MAX_BYTES + 1];
    size_t first_length = 0;
    size_t second_length = 0;
    cn_settings_result result;
    int system_errno;
    int failures = 0;
    size_t i;

    if (!workspace || workspace[0] != '/' ||
        join_path(root, sizeof root, workspace, "root") != 0 ||
        join_path(outside, sizeof outside, workspace, "outside-sentinel") != 0 ||
        mkdir(root, 0700) != 0 ||
        write_bytes(outside, outside_text, sizeof outside_text - 1) != 0 ||
        !compose_store(root, &layout, &store, config) ||
        join_path(settings_path, sizeof settings_path, config,
                  "settings.conf") != 0 ||
        snprintf(temp_path, sizeof temp_path, "%s.tmp.%ld", settings_path,
                 (long)getpid()) >= (int)sizeof temp_path) {
        fprintf(stderr, "settings smoke fixture setup failed\n");
        return 2;
    }

    cn_settings_defaults(&defaults);
    check(defaults.kosync_enabled == 0 &&
              defaults.kosync_base_url[0] == '\0' &&
              strcmp(defaults.kosync_device_name, "CrossNook") == 0 &&
              cn_settings_validate(&defaults) == CN_SETTINGS_OK,
          "exact defaults are valid and KOSync is disabled", &failures);
    cn_settings_defaults(NULL);
    check(1, "defaults NULL is harmless", &failures);
    check(cn_settings_validate(NULL) == CN_SETTINGS_INVALID_ARGUMENT,
          "validate rejects NULL", &failures);

    memset(&loaded, 0xa5, sizeof loaded);
    result = cn_settings_load(&store, &loaded, &system_errno);
    check(result == CN_SETTINGS_MISSING && system_errno == ENOENT &&
              is_defaults(&loaded) && path_missing(settings_path),
          "MISSING returns defaults without creating a file", &failures);
    check(cn_settings_save(&store, &defaults, &system_errno) ==
              CN_SETTINGS_OK &&
              file_equals(settings_path, default_file,
                          sizeof default_file - 1) &&
              file_mode_is(settings_path, 0600),
          "default serialization matches the independent golden and mode 0600",
          &failures);

    memcpy(mutable_config, config, strlen(config) + 1);
    check(cn_settings_store_init(&copied_store, mutable_config,
                                 &system_errno) == CN_SETTINGS_OK,
          "caller CONFIG buffer initializes", &failures);
    memset(mutable_config, 'x', strlen(mutable_config));
    check(strcmp(copied_store.config_directory, config) == 0,
          "store owns its CONFIG directory copy", &failures);

    set_nondefault(&new_settings);
    check(cn_settings_save(&store, &new_settings, &system_errno) ==
              CN_SETTINGS_OK && system_errno == 0,
          "valid v1 settings save", &failures);
    check(cn_settings_load(&store, &loaded, &system_errno) == CN_SETTINGS_OK &&
              settings_equal(&loaded, &new_settings),
          "save and load round trip", &failures);
    check(read_bytes(settings_path, first, sizeof first, &first_length) == 0 &&
              verify_crc_line(first, first_length),
          "serialized CRC covers the deterministic body", &failures);
    check(cn_settings_save(&store, &new_settings, &system_errno) ==
              CN_SETTINGS_OK &&
              read_bytes(settings_path, second, sizeof second,
                         &second_length) == 0 &&
              first_length == second_length &&
              memcmp(first, second, first_length) == 0,
          "repeated save is byte-identical", &failures);

    check(fixture_loads_as(&store, settings_path, unknown_body,
                           sizeof unknown_body - 1, CN_SETTINGS_CORRUPT),
          "unknown field is corrupt", &failures);
    check(fixture_loads_as(&store, settings_path, duplicate_body,
                           sizeof duplicate_body - 1, CN_SETTINGS_CORRUPT),
          "duplicate field is corrupt", &failures);
    check(fixture_loads_as(&store, settings_path, missing_body,
                           sizeof missing_body - 1, CN_SETTINGS_CORRUPT),
          "missing field is corrupt", &failures);
    check(fixture_loads_as(&store, settings_path, malformed_line_body,
                           sizeof malformed_line_body - 1,
                           CN_SETTINGS_CORRUPT),
          "malformed line is corrupt", &failures);
    check(fixture_loads_as(&store, settings_path, malformed_version_body,
                           sizeof malformed_version_body - 1,
                           CN_SETTINGS_CORRUPT),
          "noncanonical version is corrupt", &failures);
    check(fixture_loads_as(&store, settings_path, future_body,
                           sizeof future_body - 1,
                           CN_SETTINGS_UNSUPPORTED_VERSION),
          "future version is unsupported with defaults", &failures);
    check(fixture_loads_as(&store, settings_path, invalid_bool_body,
                           sizeof invalid_bool_body - 1,
                           CN_SETTINGS_CORRUPT),
          "invalid boolean is corrupt", &failures);
    check(fixture_loads_as(&store, settings_path, invalid_url_body,
                           sizeof invalid_url_body - 1,
                           CN_SETTINGS_CORRUPT),
          "invalid URL is corrupt", &failures);
    check(fixture_loads_as(&store, settings_path, invalid_utf8_body,
                           sizeof invalid_utf8_body - 1,
                           CN_SETTINGS_CORRUPT),
          "invalid UTF-8 device name is corrupt", &failures);

    cn_settings_defaults(&boundary);
    boundary.kosync_enabled = 1;
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "enabled KOSync requires a base URL", &failures);
    memcpy(boundary.kosync_base_url, "https://user@sync.example.test",
           sizeof "https://user@sync.example.test");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "URL userinfo is rejected", &failures);
    memcpy(boundary.kosync_base_url, "https://sync.example.test?q=1",
           sizeof "https://sync.example.test?q=1");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "URL query is rejected", &failures);
    memcpy(boundary.kosync_base_url, "https://sync.example.test#fragment",
           sizeof "https://sync.example.test#fragment");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "URL fragment is rejected", &failures);
    memcpy(boundary.kosync_base_url, "https://sync.example.test/base\\path",
           sizeof "https://sync.example.test/base\\path");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "URL backslash is rejected like the transport parser", &failures);
    memcpy(boundary.kosync_base_url, "https://sync.example.test:65536",
           sizeof "https://sync.example.test:65536");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "URL port outside the KOSync bound is rejected", &failures);
    memcpy(boundary.kosync_base_url, "https://sync.example.test/bad path",
           sizeof "https://sync.example.test/bad path");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "URL whitespace is rejected", &failures);
    memcpy(boundary.kosync_base_url, "https://sync.example.test/\x80",
           sizeof "https://sync.example.test/\x80");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "URL non-ASCII byte is rejected", &failures);
    cn_settings_defaults(&boundary);
    boundary.kosync_device_name[0] = 1;
    boundary.kosync_device_name[1] = '\0';
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "device-name control character is rejected", &failures);
    cn_settings_defaults(&boundary);
    memcpy(boundary.kosync_device_name, "CrossNook \xd0\xa2\xd0\xb5\xd1\x81\xd1\x82",
           sizeof "CrossNook \xd0\xa2\xd0\xb5\xd1\x81\xd1\x82");
    check(cn_settings_validate(&boundary) == CN_SETTINGS_OK,
          "valid non-ASCII UTF-8 device name is accepted", &failures);
    boundary.kosync_device_name[0] = 0x7f;
    boundary.kosync_device_name[1] = '\0';
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "device-name DEL control is rejected", &failures);
    boundary.kosync_device_name[0] = (char)0xc2;
    boundary.kosync_device_name[1] = (char)0x85;
    boundary.kosync_device_name[2] = '\0';
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "device-name C1 control is rejected", &failures);

    cn_settings_defaults(&boundary);
    boundary.kosync_enabled = 1;
    memcpy(boundary.kosync_base_url, "https://", 8);
    memset(boundary.kosync_base_url + 8, 'a', 255);
    boundary.kosync_base_url[263] = '/';
    memset(boundary.kosync_base_url + 264, 'b', 1023);
    boundary.kosync_base_url[1287] = '\0';
    memset(boundary.kosync_device_name, 'd', CN_SETTINGS_DEVICE_NAME_MAX);
    boundary.kosync_device_name[CN_SETTINGS_DEVICE_NAME_MAX] = '\0';
    check(cn_settings_validate(&boundary) == CN_SETTINGS_OK &&
              cn_settings_save(&store, &boundary, &system_errno) ==
                  CN_SETTINGS_OK &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_OK && settings_equal(&loaded, &boundary),
          "exact host path and device bounds round trip", &failures);
    boundary.kosync_base_url[1287] = 'b';
    boundary.kosync_base_url[1288] = '\0';
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "one byte over base-path bound is rejected", &failures);
    cn_settings_defaults(&boundary);
    memset(boundary.kosync_device_name, 'd',
           sizeof boundary.kosync_device_name);
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "unterminated maximum device field is rejected", &failures);
    cn_settings_defaults(&boundary);
    memset(boundary.kosync_base_url, 'u',
           sizeof boundary.kosync_base_url);
    check(cn_settings_validate(&boundary) == CN_SETTINGS_INVALID_SETTINGS,
          "unterminated maximum URL field is rejected", &failures);

    check(cn_settings_save(&store, &new_settings, &system_errno) ==
              CN_SETTINGS_OK &&
              read_bytes(settings_path, first, sizeof first, &first_length) ==
                  0,
          "corruption source file prepared", &failures);
    check(first_length > 1 &&
              write_bytes(settings_path, first, first_length - 1) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_CORRUPT && is_defaults(&loaded),
          "truncated file is corrupt", &failures);
    second_length = first_length;
    memcpy(second, first, first_length);
    second[10] = '\0';
    check(write_bytes(settings_path, second, second_length) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_CORRUPT && is_defaults(&loaded),
          "embedded NUL is corrupt", &failures);
    memcpy(second, first, first_length);
    second[35] ^= 1;
    check(write_bytes(settings_path, second, first_length) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_CORRUPT && is_defaults(&loaded),
          "CRC mismatch is corrupt", &failures);
    memcpy(second, first, first_length);
    second[first_length] = 'x';
    check(write_bytes(settings_path, second, first_length + 1) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_CORRUPT && is_defaults(&loaded),
          "trailing data is corrupt", &failures);
    memset(second, 'x', sizeof second);
    check(write_bytes(settings_path, second, sizeof second) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_CORRUPT && is_defaults(&loaded),
          "file one byte over the whole-file bound is corrupt", &failures);

    check(write_bytes(temp_path, "stale\n", 6) == 0 &&
              cn_settings_save(&store, &new_settings, &system_errno) ==
                  CN_SETTINGS_OK && path_missing(temp_path),
          "stale process temp is replaced and cleaned", &failures);

    check(unlink(settings_path) == 0 &&
              symlink(outside, settings_path) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_SYMLINK && is_defaults(&loaded) &&
              cn_settings_save(&store, &new_settings, &system_errno) ==
                  CN_SETTINGS_SYMLINK &&
              file_equals(outside, outside_text, sizeof outside_text - 1),
          "final settings symlink is rejected on load and save", &failures);
    (void)unlink(settings_path);
    check(mkdir(settings_path, 0700) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_NON_REGULAR &&
              cn_settings_save(&store, &new_settings, &system_errno) ==
                  CN_SETTINGS_NON_REGULAR,
          "settings directory is rejected", &failures);
    (void)rmdir(settings_path);
    check(mkfifo(settings_path, 0600) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_NON_REGULAR &&
              cn_settings_save(&store, &new_settings, &system_errno) ==
                  CN_SETTINGS_NON_REGULAR,
          "settings FIFO is rejected without opening", &failures);
    (void)unlink(settings_path);

    cn_settings_defaults(&old_settings);
    check(fault_preserves_old(&store, &old_settings, &new_settings,
                              FAULT_WRITE_NOSPC, CN_SETTINGS_NO_SPACE,
                              ENOSPC),
          "ENOSPC preserves old settings", &failures);
#ifdef EDQUOT
    check(fault_preserves_old(&store, &old_settings, &new_settings,
                              FAULT_WRITE_QUOTA, CN_SETTINGS_NO_SPACE,
                              EDQUOT),
          "EDQUOT preserves old settings", &failures);
#endif
    check(fault_preserves_old(&store, &old_settings, &new_settings,
                              FAULT_TEMP_FSYNC, CN_SETTINGS_IO_ERROR, EIO),
          "temp fsync failure preserves old settings", &failures);
    check(fault_preserves_old(&store, &old_settings, &new_settings,
                              FAULT_TEMP_CLOSE, CN_SETTINGS_IO_ERROR, EIO),
          "temp close failure preserves old settings", &failures);
    check(fault_preserves_old(&store, &old_settings, &new_settings,
                              FAULT_RENAME, CN_SETTINGS_IO_ERROR, EIO),
          "rename failure preserves old settings", &failures);

    check(cn_settings_save(&store, &old_settings, &system_errno) ==
              CN_SETTINGS_OK,
          "durability fixture old value saved", &failures);
    set_fault(FAULT_DIRECTORY_FSYNC);
    result = cn_settings_save(&store, &new_settings, &system_errno);
    clear_fault();
    check(result == CN_SETTINGS_DURABILITY_UNCERTAIN &&
              system_errno == EIO &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_OK && settings_equal(&loaded, &new_settings),
          "directory fsync uncertainty leaves new value authoritative",
          &failures);

    check(concurrent_visibility(&store, &old_settings, &new_settings),
          "concurrent reader sees only complete old or new settings",
          &failures);

    if (cn_settings_store_init(&copied_store, "/sys", &system_errno) ==
        CN_SETTINGS_OK) {
        result = cn_settings_save(&copied_store, &defaults, &system_errno);
        check(result == CN_SETTINGS_READ_ONLY ||
                  result == CN_SETTINGS_PERMISSION_DENIED,
              "read-only or protected storage fails explicitly", &failures);
    } else {
        printf("[SKIP] /sys is unavailable for read-only probing\n");
    }

    check(join_path(other, sizeof other, workspace, "not-directory") == 0 &&
              write_bytes(other, "x", 1) == 0 &&
              cn_settings_store_init(&copied_store, other, &system_errno) ==
                  CN_SETTINGS_NOT_DIRECTORY,
          "non-directory CONFIG is rejected", &failures);
    (void)unlink(other);
    check(join_path(other, sizeof other, workspace, "config-link") == 0 &&
              symlink(config, other) == 0 &&
              cn_settings_store_init(&copied_store, other, &system_errno) ==
                  CN_SETTINGS_SYMLINK,
          "final CONFIG symlink is rejected", &failures);
    (void)unlink(other);
    check(join_path(other, sizeof other, workspace, "missing-config") == 0 &&
              cn_settings_store_init(&copied_store, other, &system_errno) ==
                  CN_SETTINGS_NOT_FOUND && system_errno == ENOENT,
          "missing CONFIG is rejected", &failures);

    check(cn_settings_load(NULL, &loaded, &system_errno) ==
              CN_SETTINGS_INVALID_ARGUMENT &&
              cn_settings_load(&store, NULL, &system_errno) ==
                  CN_SETTINGS_INVALID_ARGUMENT &&
              cn_settings_save(NULL, &defaults, &system_errno) ==
                  CN_SETTINGS_INVALID_ARGUMENT &&
              cn_settings_save(&store, NULL, &system_errno) ==
                  CN_SETTINGS_INVALID_ARGUMENT,
          "NULL API arguments are rejected", &failures);
    check(strcmp(cn_settings_result_name((cn_settings_result)-1), "unknown") ==
              0 &&
              strcmp(cn_settings_result_name(CN_SETTINGS_RESULT_COUNT),
                     "unknown") == 0,
          "result name bounds fail closed", &failures);
    check(file_equals(outside, outside_text, sizeof outside_text - 1),
          "all operations preserve the outside sentinel", &failures);
    check(read_bytes(settings_path, first, sizeof first, &first_length) == 0 &&
              serialized_keys_are_exact(first, first_length) &&
              memmem(first, first_length, "password", 8) == NULL &&
              memmem(first, first_length, "userkey", 7) == NULL &&
              memmem(first, first_length, "token", 5) == NULL &&
              memmem(first, first_length, "device_id", 9) == NULL &&
              memmem(first, first_length, "wifi", 4) == NULL &&
              memmem(first, first_length, "books", 5) == NULL &&
              memmem(first, first_length, "dns", 3) == NULL &&
              memmem(first, first_length, "sntp", 4) == NULL,
          "serialization contains exactly the approved schema keys",
          &failures);

    for (i = 0; i < sizeof first; ++i)
        first[i] = 0;
    printf("SETTINGS STORE SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_expect_save(const char *config, const char *expected)
{
    cn_settings_store store;
    cn_settings settings;
    cn_settings_result result;
    int system_errno;
    if (cn_settings_store_init(&store, config, &system_errno) != CN_SETTINGS_OK)
        return 1;
    cn_settings_defaults(&settings);
    result = cn_settings_save(&store, &settings, &system_errno);
    printf("SETTINGS SAVE result=%s errno=%d\n",
           cn_settings_result_name(result), system_errno);
    return strcmp(cn_settings_result_name(result), expected) == 0 ? 0 : 1;
}

static int outside_ok(const char *path)
{
    return file_equals(path, "outside\n", sizeof "outside\n" - 1);
}

static int physical_store(const char *root, cn_settings_store *store,
                          char config[CN_STORAGE_PATH_CAPACITY])
{
    cn_storage_layout layout;
    return compose_store(root, &layout, store, config);
}

static int run_physical_missing(const char *root, const char *outside)
{
    cn_settings_store store;
    cn_settings loaded;
    char config[CN_STORAGE_PATH_CAPACITY];
    char path[CN_STORAGE_PATH_CAPACITY];
    int system_errno;
    int failures = 0;
    if (!physical_store(root, &store, config)) {
        check(0, "physical Storage Layout CONFIG composes", &failures);
        return 1;
    }
    check(1, "physical Storage Layout CONFIG composes", &failures);
    check(join_path(path, sizeof path, config, "settings.conf") == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_MISSING && is_defaults(&loaded) &&
              path_missing(path),
          "physical missing settings returns exact defaults without creation",
          &failures);
    check(outside_ok(outside), "physical outside sentinel unchanged",
          &failures);
    printf("SETTINGS physical-missing config=%s result=%s defaults=%s\n",
           config, failures == 0 ? "missing" : "failed",
           failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}

static int run_physical_save(const char *root, const char *outside,
                             const char *url, const char *device)
{
    cn_settings_store store;
    cn_settings settings;
    char config[CN_STORAGE_PATH_CAPACITY];
    char path[CN_STORAGE_PATH_CAPACITY];
    unsigned char first[CN_SETTINGS_FILE_MAX_BYTES + 1];
    unsigned char second[CN_SETTINGS_FILE_MAX_BYTES + 1];
    size_t first_length = 0;
    size_t second_length = 0;
    int system_errno;
    int failures = 0;
    if (!physical_store(root, &store, config)) {
        check(0, "physical save composes CONFIG", &failures);
        return 1;
    }
    check(1, "physical save composes CONFIG", &failures);
    cn_settings_defaults(&settings);
    settings.kosync_enabled = 1;
    if (strlen(url) < sizeof settings.kosync_base_url)
        memcpy(settings.kosync_base_url, url, strlen(url) + 1);
    if (strlen(device) < sizeof settings.kosync_device_name)
        memcpy(settings.kosync_device_name, device, strlen(device) + 1);
    check(cn_settings_save(&store, &settings, &system_errno) ==
              CN_SETTINGS_OK,
          "physical non-secret settings save", &failures);
    join_path(path, sizeof path, config, "settings.conf");
    check(read_bytes(path, first, sizeof first, &first_length) == 0 &&
              cn_settings_save(&store, &settings, &system_errno) ==
                  CN_SETTINGS_OK &&
              read_bytes(path, second, sizeof second, &second_length) == 0 &&
              first_length == second_length &&
              memcmp(first, second, first_length) == 0,
          "physical repeated save is deterministic", &failures);
    check(outside_ok(outside), "physical outside sentinel unchanged",
          &failures);
    printf("SETTINGS physical-save enabled=1 url=%s device=%s deterministic=%s\n",
           url, device, failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}

static int run_physical_load(const char *root, const char *outside,
                             const char *url, const char *device)
{
    cn_settings_store store;
    cn_settings loaded;
    char config[CN_STORAGE_PATH_CAPACITY];
    int system_errno;
    int failures = 0;
    if (!physical_store(root, &store, config)) {
        check(0, "physical reload composes CONFIG", &failures);
        return 1;
    }
    check(1, "physical reload composes CONFIG", &failures);
    check(cn_settings_load(&store, &loaded, &system_errno) == CN_SETTINGS_OK &&
              loaded.kosync_enabled == 1 &&
              strcmp(loaded.kosync_base_url, url) == 0 &&
              strcmp(loaded.kosync_device_name, device) == 0,
          "physical fresh invocation reloads exact settings", &failures);
    check(outside_ok(outside), "physical outside sentinel unchanged",
          &failures);
    printf("SETTINGS physical-load enabled=%d url=%s device=%s\n",
           loaded.kosync_enabled, loaded.kosync_base_url,
           loaded.kosync_device_name);
    return failures == 0 ? 0 : 1;
}

static int run_physical_corrupt(const char *root, const char *outside)
{
    cn_settings_store store;
    cn_settings loaded;
    char config[CN_STORAGE_PATH_CAPACITY];
    char path[CN_STORAGE_PATH_CAPACITY];
    unsigned char corrupt[CN_SETTINGS_FILE_MAX_BYTES + 1];
    size_t length = 0;
    int system_errno;
    int failures = 0;
    if (!physical_store(root, &store, config)) {
        check(0, "physical corruption composes CONFIG", &failures);
        return 1;
    }
    check(1, "physical corruption composes CONFIG", &failures);
    join_path(path, sizeof path, config, "settings.conf");
    check(read_bytes(path, corrupt, sizeof corrupt, &length) == 0 &&
              length > 40,
          "physical saved file captured", &failures);
    if (length <= 40)
        return 1;
    corrupt[35] ^= 1;
    check(write_bytes(path, corrupt, length) == 0 &&
              cn_settings_load(&store, &loaded, &system_errno) ==
                  CN_SETTINGS_CORRUPT && is_defaults(&loaded),
          "physical corruption returns exact defaults", &failures);
    check(file_equals(path, corrupt, length),
          "physical corrupt file is not overwritten", &failures);
    check(outside_ok(outside), "physical outside sentinel unchanged",
          &failures);
    printf("SETTINGS physical-corrupt result=%s defaults=%s preserved=%s\n",
           failures == 0 ? "corrupt" : "failed",
           failures == 0 ? "ok" : "failed",
           failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--smoke") == 0)
        return run_smoke(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--expect-save") == 0)
        return run_expect_save(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--physical-missing") == 0)
        return run_physical_missing(argv[2], argv[3]);
    if (argc == 6 && strcmp(argv[1], "--physical-save") == 0)
        return run_physical_save(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 6 && strcmp(argv[1], "--physical-load") == 0)
        return run_physical_load(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 4 && strcmp(argv[1], "--physical-corrupt") == 0)
        return run_physical_corrupt(argv[2], argv[3]);
    fprintf(stderr,
            "usage: crossnook-settings-test --smoke <workspace>\n"
            "       crossnook-settings-test --expect-save <config> <result>\n"
            "       crossnook-settings-test --physical-missing <root> <outside>\n"
            "       crossnook-settings-test --physical-save <root> <outside> <url> <device>\n"
            "       crossnook-settings-test --physical-load <root> <outside> <url> <device>\n"
            "       crossnook-settings-test --physical-corrupt <root> <outside>\n");
    return 2;
}
