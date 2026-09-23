/* Synthetic credential-store host and later verified-card diagnostic. */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "credentials/credential_store.h"
#include "platform/storage_verify.h"

/* These identifiers are fixtures, never user-provided or printed. */
#define DUMMY_USER "crossnook-synthetic-account-only"
#define DUMMY_KEY "crossnook-synthetic-nonreal-key-only"
#define SECOND_KEY "crossnook-synthetic-alternate-only"

enum fault { NONE, TEMP_OPEN_FAIL, WRITE_FAIL, FILE_FSYNC_FAIL, CLOSE_FAIL,
             RENAME_FAIL, DIR_OPEN_FAIL, DIR_FSYNC_FAIL, DIR_UNSUPPORTED,
             DIR_CLOSE_FAIL };
static enum fault fault;
static int fsync_calls, close_calls;

ssize_t __real_write(int, const void *, size_t);
int __real_open(const char *, int, ...);
int __real_fsync(int);
int __real_close(int);
int __real_rename(const char *, const char *);
int __wrap_open(const char *path, int flags, ...)
{
    if ((fault == TEMP_OPEN_FAIL && (flags & O_EXCL)) ||
        (fault == DIR_OPEN_FAIL && (flags & O_DIRECTORY))) {
        errno = EIO;
        return -1;
    }
    if (flags & O_CREAT) return __real_open(path, flags, (mode_t)0600);
    return __real_open(path, flags);
}
ssize_t __wrap_write(int fd, const void *buf, size_t count)
{
    if (fault == WRITE_FAIL) { errno = ENOSPC; return -1; }
    return __real_write(fd, buf, count);
}
int __wrap_fsync(int fd)
{
    ++fsync_calls;
    if ((fault == FILE_FSYNC_FAIL && fsync_calls == 1) ||
        ((fault == DIR_FSYNC_FAIL || fault == DIR_UNSUPPORTED) && fsync_calls == 2)) {
        errno = fault == DIR_UNSUPPORTED ? EINVAL : EIO;
        return -1;
    }
    return __real_fsync(fd);
}
int __wrap_close(int fd)
{
    int rc = __real_close(fd);
    ++close_calls;
    if ((fault == CLOSE_FAIL && close_calls == 1) ||
        (fault == DIR_CLOSE_FAIL && close_calls == 2)) {
        errno = EIO; return -1;
    }
    return rc;
}
int __wrap_rename(const char *from, const char *to)
{
    if (fault == RENAME_FAIL) { errno = EIO; return -1; }
    return __real_rename(from, to);
}

static int failures;
static void clear_bytes(void *data, size_t size)
{
    volatile unsigned char *p = (volatile unsigned char *)data;
    while (size--) *p++ = 0;
}
static void check(int ok, const char *name)
{
    printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    if (!ok) ++failures;
}
static void set_fault(enum fault value)
{
    fault = value; fsync_calls = 0; close_calls = 0;
}
static int write_raw(const char *path, const void *data, size_t size)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    int ok;
    if (fd < 0) return 0;
    ok = __real_write(fd, data, size) == (ssize_t)size;
    return __real_close(fd) == 0 && ok;
}
static int read_raw(const char *path, unsigned char *out, size_t cap, size_t *size)
{
    int fd = open(path, O_RDONLY), n;
    if (fd < 0) return 0;
    n = (int)read(fd, out, cap);
    if (__real_close(fd) || n < 0) return 0;
    *size = (size_t)n;
    return 1;
}
static uint32_t crc32(const unsigned char *data, size_t size)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    int bit;
    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
}
static int fixture(const char *path, const char *body)
{
    char record[CN_CREDENTIAL_FILE_MAX_BYTES + 1];
    size_t n = strlen(body);
    int m;
    if (n > sizeof record - 16) return 0;
    memcpy(record, body, n);
    m = snprintf(record + n, sizeof record - n, "crc32=%08x\n", crc32((const unsigned char *)body, n));
    return m > 0 && (size_t)m < sizeof record - n && write_raw(path, record, n + (size_t)m);
}
static int empty(const cn_credentials *credentials)
{
    const unsigned char *p = (const unsigned char *)credentials;
    size_t i;
    for (i = 0; i < sizeof *credentials; ++i)
        if (p[i]) return 0;
    return 1;
}
static int no_temp_files(const char *directory)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    int ok = dir != NULL;
    if (!dir) return 0;
    while ((entry = readdir(dir)) != NULL)
        if (strncmp(entry->d_name, "credentials.tmp.", sizeof "credentials.tmp." - 1) == 0)
            ok = 0;
    closedir(dir);
    return ok;
}
static void expect_load(cn_credential_store *store, cn_credential_result expected,
                        const char *name)
{
    cn_credentials got;
    cn_credential_result result;
    memset(&got, 0xa5, sizeof got);
    result = cn_credential_store_load(store, &got, NULL);
    check(result == expected && (result == CN_CREDENTIAL_OK || empty(&got)), name);
    cn_credentials_clear(&got);
}
static void synthetic(cn_credentials *credentials, int alternate)
{
    memset(credentials, 0, sizeof *credentials);
    strcpy(credentials->username, DUMMY_USER);
    strcpy(credentials->userkey, alternate ? SECOND_KEY : DUMMY_KEY);
}
static int correct(cn_credential_store *store, int alternate)
{
    cn_credentials got, wanted;
    int ok;
    synthetic(&wanted, alternate);
    memset(&got, 0xa5, sizeof got);
    ok = cn_credential_store_load(store, &got, NULL) == CN_CREDENTIAL_OK &&
         memcmp(&got, &wanted, sizeof got) == 0;
    cn_credentials_clear(&got);
    cn_credentials_clear(&wanted);
    return ok;
}
static int smoke(const char *directory)
{
    cn_credential_store store;
    cn_credentials value;
    char path[CN_CREDENTIAL_PATH_CAPACITY], other[CN_CREDENTIAL_PATH_CAPACITY];
    unsigned char original[CN_CREDENTIAL_FILE_MAX_BYTES], next[CN_CREDENTIAL_FILE_MAX_BYTES];
    size_t length = 0, length2 = 0;
    struct stat st;
    int i;
    const enum fault pre[] = {TEMP_OPEN_FAIL, WRITE_FAIL, FILE_FSYNC_FAIL,
                              CLOSE_FAIL, RENAME_FAIL};
    const enum fault post[] = {DIR_OPEN_FAIL, DIR_FSYNC_FAIL, DIR_UNSUPPORTED,
                               DIR_CLOSE_FAIL};
    check(cn_credential_store_init(&store, directory, NULL) == CN_CREDENTIAL_OK, "supplied existing directory");
    if (failures) return 1;
    snprintf(path, sizeof path, "%s/credentials", directory);
    snprintf(other, sizeof other, "%s/sentinel", directory);
    check(write_raw(other, "untouched", 9), "sentinel fixture");
    expect_load(&store, CN_CREDENTIAL_MISSING, "missing clears output without creating final file");
    check(lstat(path, &st) < 0 && errno == ENOENT, "missing does not create file");
    synthetic(&value, 0);
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_OK && correct(&store, 0),
          "save and exact synthetic reload");
    check(read_raw(path, original, sizeof original, &length), "read deterministic fixture");
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_OK &&
          read_raw(path, next, sizeof next, &length2) && length == length2 &&
          memcmp(original, next, length) == 0, "deterministic serialization");
    check(write_raw(path, original, length - 1), "truncated fixture");
    expect_load(&store, CN_CREDENTIAL_CORRUPT, "truncated file clears output");
    memcpy(next, original, length);
    next[10] ^= 1;
    check(write_raw(path, next, length), "corrupt checksum fixture");
    expect_load(&store, CN_CREDENTIAL_CORRUPT, "corrupt content clears output");
    check(fixture(path, "crossnook-credentials=2\nusername=x\nuserkey=y\n"), "future version fixture");
    check(read_raw(path, original, sizeof original, &length), "snapshot future version");
    expect_load(&store, CN_CREDENTIAL_UNSUPPORTED_VERSION, "unsupported version clears output");
    check(read_raw(path, next, sizeof next, &length2) && length == length2 &&
          memcmp(original, next, length) == 0,
          "unsupported file preserved without automatic rewrite");
    check(fixture(path, "crossnook-credentials=1\nusername=x\nusername=z\nuserkey=y\n"), "duplicate fixture");
    check(read_raw(path, original, sizeof original, &length), "snapshot corrupt file");
    expect_load(&store, CN_CREDENTIAL_CORRUPT, "duplicate field rejected");
    check(read_raw(path, next, sizeof next, &length2) && length == length2 &&
          memcmp(original, next, length) == 0,
          "corrupt file preserved without automatic rewrite");
    check(fixture(path, "crossnook-credentials=1\nusername=x\nunknown=y\nuserkey=y\n"), "unknown fixture");
    expect_load(&store, CN_CREDENTIAL_CORRUPT, "unknown field rejected");
    check(fixture(path, "crossnook-credentials=1\nusername=x\n"), "missing field fixture");
    expect_load(&store, CN_CREDENTIAL_CORRUPT, "missing field rejected");
    {
        char body[CN_CREDENTIAL_FILE_MAX_BYTES];
        size_t prefix = strlen("crossnook-credentials=1\nusername=");
        memcpy(body, "crossnook-credentials=1\nusername=", prefix);
        memset(body + prefix, 'a', CN_CREDENTIAL_USERNAME_MAX + 1);
        strcpy(body + prefix + CN_CREDENTIAL_USERNAME_MAX + 1, "\nuserkey=x\n");
        check(fixture(path, body), "oversized username on-disk fixture");
        expect_load(&store, CN_CREDENTIAL_CORRUPT, "oversized on-disk username rejected");
        strcpy(body, "crossnook-credentials=1\nusername=x\nuserkey=");
        prefix = strlen(body);
        memset(body + prefix, 'b', CN_CREDENTIAL_USERKEY_MAX + 1);
        strcpy(body + prefix + CN_CREDENTIAL_USERKEY_MAX + 1, "\n");
        check(fixture(path, body), "oversized userkey on-disk fixture");
        expect_load(&store, CN_CREDENTIAL_CORRUPT, "oversized on-disk userkey rejected");
    }
    cn_credentials_clear(&value);
    synthetic(&value, 0);
    memset(value.username, 'u', sizeof value.username);
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_INVALID,
          "oversized username rejected");
    synthetic(&value, 0);
    memset(value.userkey, 'k', sizeof value.userkey);
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_INVALID,
          "oversized userkey rejected");
    synthetic(&value, 0);
    value.username[1] = '\r';
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_INVALID,
          "control character rejected");
    synthetic(&value, 0);
    check(fixture(path, "crossnook-credentials=1\nusername=x\nuserkey=bad\rkey\n"), "control fixture");
    expect_load(&store, CN_CREDENTIAL_CORRUPT, "control character on disk rejected");
    {
        static const char embedded[] =
            "crossnook-credentials=1\nusername=x\nuserkey=a\0b\ncrc32=12345678\n";
        check(write_raw(path, embedded, sizeof embedded - 1), "embedded NUL fixture");
    }
    expect_load(&store, CN_CREDENTIAL_CORRUPT, "embedded NUL rejected");
    unlink(path);
    check(symlink(other, path) == 0, "symlink fixture");
    expect_load(&store, CN_CREDENTIAL_SYMLINK, "symlink load rejected");
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_SYMLINK,
          "symlink save rejected");
    unlink(path);
    check(mkdir(path, 0700) == 0, "directory fixture");
    expect_load(&store, CN_CREDENTIAL_NON_REGULAR, "directory load rejected");
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_NON_REGULAR,
          "directory save rejected");
    rmdir(path);
    check(mkfifo(path, 0600) == 0, "FIFO fixture");
    expect_load(&store, CN_CREDENTIAL_NON_REGULAR, "FIFO load rejected without blocking");
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_NON_REGULAR,
          "FIFO save rejected");
    unlink(path);
    check(cn_credential_store_save(&store, &value, NULL) == CN_CREDENTIAL_OK, "restore old credential fixture");
    set_fault(CLOSE_FAIL);
    expect_load(&store, CN_CREDENTIAL_IO_ERROR, "load close failure clears output");
    set_fault(NONE);
    synthetic(&value, 1);
    for (i = 0; i < (int)(sizeof pre / sizeof pre[0]); ++i) {
        cn_credential_result result;
        set_fault(pre[i]);
        result = cn_credential_store_save(&store, &value, NULL);
        set_fault(NONE);
        check(result != CN_CREDENTIAL_OK && result != CN_CREDENTIAL_DURABILITY_UNCERTAIN &&
              correct(&store, 0) && no_temp_files(directory),
              "pre-rename failure preserves old value and cleans temp");
    }
    for (i = 0; i < (int)(sizeof post / sizeof post[0]); ++i) {
        cn_credential_result result;
        set_fault(post[i]);
        result = cn_credential_store_save(&store, &value, NULL);
        set_fault(NONE);
        check(result == CN_CREDENTIAL_DURABILITY_UNCERTAIN && correct(&store, 1) &&
              no_temp_files(directory),
              "post-rename failure keeps new visible value");
    }
    check(read_raw(other, next, sizeof next, &length2) && length2 == 9 &&
          memcmp(next, "untouched", 9) == 0, "outside credential file sentinel unchanged");
    cn_credentials_clear(&value);
    clear_bytes(original, sizeof original);
    clear_bytes(next, sizeof next);
    printf("CREDENTIAL STORE SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int physical(const char *phase, const char *mount, const char *root,
                    const char *major_arg, const char *minor_arg)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_credential_store store;
    cn_credentials value;
    cn_credential_result result;
    char directory[CN_CREDENTIAL_PATH_CAPACITY], path[CN_CREDENTIAL_PATH_CAPACITY];
    char *end;
    struct stat root_st, state_st, before, opened;
    unsigned char damaged[CN_CREDENTIAL_FILE_MAX_BYTES];
    unsigned char reloaded[CN_CREDENTIAL_FILE_MAX_BYTES];
    size_t damaged_size = 0, reloaded_size = 0;
    unsigned long number;
    int fd, byte;
    if (strcmp(phase, "missing") && strcmp(phase, "save") &&
        strcmp(phase, "load") && strcmp(phase, "corrupt")) return 2;
    number = strtoul(major_arg, &end, 10);
    if (!major_arg[0] || *end || number > 0xffffffffUL) return 2;
    candidate.expected_major = (unsigned)number;
    number = strtoul(minor_arg, &end, 10);
    if (!minor_arg[0] || *end || number > 0xffffffffUL) return 2;
    candidate.expected_minor = (unsigned)number;
    candidate.root = root; candidate.mountpoint = mount;
    if (cn_platform_storage_verify(&candidate, &verified, NULL) != CN_PLATFORM_STORAGE_OK) {
        puts("CREDENTIAL GATE storage=unverified persistence=not-attempted");
        return 1;
    }
    if (snprintf(directory, sizeof directory, "%s/state", verified.root) >= (int)sizeof directory ||
        cn_credential_store_init(&store, directory, NULL) != CN_CREDENTIAL_OK ||
        stat(verified.root, &root_st) != 0 ||
        lstat(directory, &state_st) != 0 ||
        root_st.st_dev != state_st.st_dev)
        return 1;
    synthetic(&value, 0);
    if (!strcmp(phase, "missing")) {
        cn_credentials got;
        result = cn_credential_store_load(&store, &got, NULL);
        printf("CREDENTIAL GATE credential=%s cleared=%s\n", cn_credential_result_name(result),
               empty(&got) ? "yes" : "no");
        cn_credentials_clear(&got);
        cn_credentials_clear(&value);
        return result == CN_CREDENTIAL_MISSING ? 0 : 1;
    }
    if (!strcmp(phase, "save")) {
        cn_credentials prior;
        result = cn_credential_store_load(&store, &prior, NULL);
        cn_credentials_clear(&prior);
        if (result != CN_CREDENTIAL_MISSING) {
            puts("CREDENTIAL GATE save=refused-existing-or-invalid");
            cn_credentials_clear(&value);
            return 1;
        }
        result = cn_credential_store_save(&store, &value, NULL);
        printf("CREDENTIAL GATE save=%s\n", cn_credential_result_name(result));
        cn_credentials_clear(&value);
        return result == CN_CREDENTIAL_OK || result == CN_CREDENTIAL_DURABILITY_UNCERTAIN ? 0 : 1;
    }
    if (!strcmp(phase, "load")) {
        int ok = correct(&store, 0);
        puts(ok ? "CREDENTIAL GATE reload=ok" : "CREDENTIAL GATE reload=failed");
        cn_credentials_clear(&value);
        return ok ? 0 : 1;
    }
    if (snprintf(path, sizeof path, "%s/credentials", directory) >= (int)sizeof path)
        return 1;
    /* Only an explicitly verified mount can reach this intentional corruption. */
    if (!correct(&store, 0)) {
        cn_credentials_clear(&value);
        puts("CREDENTIAL GATE corrupt=refused-nonfixture");
        return 1;
    }
    if (lstat(path, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_dev != root_st.st_dev)
        return 1;
    fd = open(path, O_RDWR | O_NOFOLLOW);
    if (fd < 0) return 1;
    if (fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        before.st_dev != opened.st_dev || before.st_ino != opened.st_ino) {
        close(fd);
        return 1;
    }
    byte = 0;
    if (pwrite(fd, &byte, 1, 0) != 1 || fsync(fd) != 0) {
        close(fd);
        return 1;
    }
    if (close(fd) != 0)
        return 1;
    if (!read_raw(path, damaged, sizeof damaged, &damaged_size))
        return 1;
    result = cn_credential_store_load(&store, &value, NULL);
    byte = empty(&value) && read_raw(path, reloaded, sizeof reloaded, &reloaded_size) &&
           damaged_size == reloaded_size &&
           memcmp(damaged, reloaded, damaged_size) == 0;
    printf("CREDENTIAL GATE corrupt=%s cleared-and-preserved=%s\n",
           cn_credential_result_name(result), byte ? "yes" : "no");
    clear_bytes(damaged, sizeof damaged);
    clear_bytes(reloaded, sizeof reloaded);
    cn_credentials_clear(&value);
    return result == CN_CREDENTIAL_CORRUPT && byte ? 0 : 1;
}

static int contains(const unsigned char *text, size_t size, const char *needle)
{
    size_t i, n = strlen(needle);
    if (n > size) return 0;
    for (i = 0; i <= size - n; ++i)
        if (memcmp(text + i, needle, n) == 0) return 1;
    return 0;
}

/* Inspect captured diagnostic bytes without ever printing any matching data. */
static int check_output(const char *path)
{
    unsigned char data[16384];
    unsigned char extra;
    struct stat st, opened;
    size_t done = 0;
    int fd, ok = 0;
    ssize_t n;
    if (strncmp(path, "/tmp/", sizeof "/tmp/" - 1) != 0 ||
        lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < 0 || st.st_size > (off_t)sizeof data)
        goto done;
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) goto done;
    if (fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode) ||
        opened.st_dev != st.st_dev || opened.st_ino != st.st_ino ||
        opened.st_size != st.st_size) {
        close(fd);
        goto done;
    }
    while (done < (size_t)st.st_size) {
        n = read(fd, data + done, (size_t)st.st_size - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        done += (size_t)n;
    }
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    if (done == (size_t)st.st_size && n == 0 &&
        !contains(data, done, DUMMY_USER) &&
        !contains(data, done, DUMMY_KEY) &&
        !contains(data, done, SECOND_KEY))
        ok = 1;
    if (close(fd) != 0) ok = 0;
done:
    clear_bytes(data, sizeof data);
    puts(ok ? "CREDENTIAL GATE redaction=ok" : "CREDENTIAL GATE redaction=failed");
    return ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--check-output"))
        return check_output(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--smoke")) {
        if (strncmp(argv[2], "/tmp/crossnook-credentials-host.",
                    sizeof "/tmp/crossnook-credentials-host." - 1) != 0)
            return 2;
        return smoke(argv[2]);
    }
    if (argc == 7 && !strcmp(argv[1], "--physical"))
        return physical(argv[2], argv[3], argv[4], argv[5], argv[6]);
    fputs("usage: credential-store-test --smoke <host-directory> | --physical <missing|save|load|corrupt> <mount> <root> <major> <minor> | --check-output </tmp/log>\n", stderr);
    return 2;
}
