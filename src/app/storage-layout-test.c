/* Portable storage layout host/device diagnostic. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "progress/progress_store.h"
#include "storage/storage_layout.h"

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

static int make_directory(const char *path, mode_t mode)
{
    return mkdir(path, mode) == 0 || errno == EEXIST ? 0 : -1;
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    while (done < length) {
        ssize_t count = write(fd, bytes + done, length - done);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        done += (size_t)count;
    }
    return 0;
}

static int write_file(const char *path, const char *text, int exclusive)
{
    int flags = O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
    int fd = open(path, flags, 0600);
    int result;
    if (fd < 0)
        return -1;
    result = write_all(fd, text, strlen(text));
    if (close(fd) != 0)
        result = -1;
    return result;
}

static int file_equals(const char *path, const char *expected)
{
    char buffer[64];
    size_t expected_length = strlen(expected);
    ssize_t count;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || expected_length >= sizeof buffer) {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    do {
        count = read(fd, buffer, sizeof buffer);
    } while (count < 0 && errno == EINTR);
    if (close(fd) != 0)
        return 0;
    return count == (ssize_t)expected_length &&
           memcmp(buffer, expected, expected_length) == 0;
}

static int is_real_directory(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           !S_ISLNK(st.st_mode);
}

static int has_mode(const char *path, mode_t mode)
{
    struct stat st;
    return lstat(path, &st) == 0 && (st.st_mode & 0777) == mode;
}

static int make_deep_root(const char *base, size_t target_length,
                          char out[CN_STORAGE_PATH_CAPACITY])
{
    size_t length = strlen(base);
    if (length >= target_length || target_length > CN_STORAGE_PATH_MAX_BYTES)
        return -1;
    memcpy(out, base, length + 1);
    while (length < target_length) {
        size_t component = target_length - length - 1;
        if (component > 200)
            component = 200;
        if (component == 0 || length + component + 1 > target_length)
            return -1;
        out[length++] = '/';
        memset(out + length, 'a', component);
        length += component;
        out[length] = '\0';
        if (mkdir(out, 0700) != 0)
            return -1;
    }
    return 0;
}

static void remove_deep_root(char *path, const char *base)
{
    size_t base_length = strlen(base);
    size_t length = strlen(path);
    while (length > base_length) {
        char *slash;
        (void)rmdir(path);
        slash = strrchr(path, '/');
        if (!slash || (size_t)(slash - path) < base_length)
            break;
        *slash = '\0';
        length = strlen(path);
    }
}

static int expect_init(const char *root, cn_storage_result expected)
{
    cn_storage_layout layout;
    int system_errno = 123;
    cn_storage_result result = cn_storage_layout_init(
        &layout, root, &system_errno);
    return result == expected &&
           (result == CN_STORAGE_OK || layout.root_length == 0);
}

static int run_smoke(const char *workspace)
{
    cn_storage_layout layout;
    cn_storage_layout trailing;
    cn_storage_layout copied;
    cn_progress_store *store = NULL;
    cn_storage_result result;
    char root[CN_STORAGE_PATH_CAPACITY];
    char root_two[CN_STORAGE_PATH_CAPACITY];
    char trailing_root[CN_STORAGE_PATH_CAPACITY];
    char config[CN_STORAGE_PATH_CAPACITY];
    char progress[CN_STORAGE_PATH_CAPACITY];
    char expected[CN_STORAGE_PATH_CAPACITY];
    char path[CN_STORAGE_PATH_CAPACITY];
    char external[CN_STORAGE_PATH_CAPACITY];
    char mutable_root[CN_STORAGE_PATH_CAPACITY];
    char deep[CN_STORAGE_PATH_CAPACITY];
    char over_derived[CN_STORAGE_PATH_CAPACITY];
    char over_capacity[CN_STORAGE_PATH_CAPACITY + 1];
    char exact[CN_STORAGE_PATH_CAPACITY];
    int system_errno;
    int failures = 0;
    size_t required;

    if (!workspace || workspace[0] != '/' || !is_real_directory(workspace)) {
        fprintf(stderr, "storage smoke requires an existing absolute workspace\n");
        return 2;
    }
    if (join_path(root, sizeof root, workspace, "root") != 0 ||
        join_path(root_two, sizeof root_two, workspace, "root-two") != 0 ||
        join_path(external, sizeof external, workspace, "external") != 0 ||
        make_directory(root, 0700) != 0 ||
        make_directory(root_two, 0700) != 0 ||
        make_directory(external, 0700) != 0) {
        fprintf(stderr, "storage smoke fixture setup failed\n");
        return 2;
    }

    check(cn_storage_layout_init(&layout, root, &system_errno) ==
              CN_STORAGE_OK && system_errno == 0,
          "existing absolute root initializes", &failures);
    if (strlen(root) + 4 > sizeof trailing_root) {
        fprintf(stderr, "storage smoke trailing-root fixture is too long\n");
        return 2;
    }
    memcpy(trailing_root, root, strlen(root));
    memcpy(trailing_root + strlen(root), "///", 4);
    check(cn_storage_layout_init(&trailing, trailing_root, &system_errno) ==
              CN_STORAGE_OK && strcmp(layout.root, trailing.root) == 0,
          "trailing slashes normalize to the same root", &failures);
    check(cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG,
                                 config, sizeof config) == CN_STORAGE_OK &&
              join_path(expected, sizeof expected, root, "config") == 0 &&
              strcmp(config, expected) == 0,
          "CONFIG path is exact", &failures);
    check(cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                                 progress, sizeof progress) == CN_STORAGE_OK &&
              join_path(expected, sizeof expected, root,
                        "state/progress") == 0 &&
              strcmp(progress, expected) == 0,
          "PROGRESS path is exact", &failures);
    check(cn_storage_layout_path(&trailing, CN_STORAGE_LOCATION_PROGRESS,
                                 path, sizeof path) == CN_STORAGE_OK &&
              strcmp(path, progress) == 0,
          "derived paths ignore supplied trailing slashes", &failures);

    required = strlen(progress) + 1;
    check(cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                                 exact, required) == CN_STORAGE_OK &&
              strcmp(exact, progress) == 0,
          "exact output capacity succeeds", &failures);
    memset(exact, 'x', sizeof exact);
    check(cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                                 exact, required - 1) ==
              CN_STORAGE_BUFFER_TOO_SMALL && exact[0] == '\0',
          "one-byte-short output fails without truncation", &failures);

    check(cn_storage_layout_init(NULL, root, &system_errno) ==
              CN_STORAGE_INVALID && system_errno == 0,
          "NULL layout is rejected", &failures);
    check(expect_init(NULL, CN_STORAGE_INVALID), "NULL root is rejected",
          &failures);
    check(expect_init("", CN_STORAGE_INVALID), "empty root is rejected",
          &failures);
    check(expect_init("relative/root", CN_STORAGE_INVALID),
          "relative root is rejected", &failures);
    check(expect_init("/", CN_STORAGE_INVALID), "filesystem root is rejected",
          &failures);
    check(expect_init("/tmp/./crossnook", CN_STORAGE_INVALID),
          "dot component is rejected", &failures);
    check(expect_init("/tmp/../crossnook", CN_STORAGE_INVALID),
          "dot-dot component is rejected", &failures);
    check(expect_init("/tmp//crossnook", CN_STORAGE_INVALID),
          "repeated internal separator is rejected", &failures);

    memset(over_derived, 'a', sizeof over_derived);
    over_derived[0] = '/';
    over_derived[CN_STORAGE_PATH_MAX_BYTES -
                 (sizeof "/state/progress" - 1) + 1] = '\0';
    check(expect_init(over_derived, CN_STORAGE_PATH_TOO_LONG),
          "one byte over derived-path limit is rejected", &failures);
    memset(over_capacity, 'a', sizeof over_capacity);
    over_capacity[0] = '/';
    over_capacity[CN_STORAGE_PATH_CAPACITY] = '\0';
    check(expect_init(over_capacity, CN_STORAGE_PATH_TOO_LONG),
          "over-capacity root is rejected", &failures);

    if (make_deep_root(workspace,
                       CN_STORAGE_PATH_MAX_BYTES -
                           (sizeof "/state/progress" - 1),
                       deep) == 0) {
        check(cn_storage_layout_init(&copied, deep, &system_errno) ==
                  CN_STORAGE_OK,
              "maximum derived-path root initializes", &failures);
        check(cn_storage_layout_path(&copied, CN_STORAGE_LOCATION_PROGRESS,
                                     path, sizeof path) == CN_STORAGE_OK &&
                  strlen(path) == CN_STORAGE_PATH_MAX_BYTES,
              "maximum derived path fits without truncation", &failures);
        remove_deep_root(deep, workspace);
    } else {
        printf("[SKIP] host filesystem did not permit maximum-length root\n");
    }

    check(join_path(path, sizeof path, workspace, "missing") == 0 &&
              cn_storage_layout_init(&copied, path, &system_errno) ==
                  CN_STORAGE_NOT_FOUND && system_errno == ENOENT,
          "missing root is distinct", &failures);
    check(join_path(path, sizeof path, workspace, "regular-root") == 0 &&
              write_file(path, "file\n", 1) == 0 &&
              cn_storage_layout_init(&copied, path, &system_errno) ==
                  CN_STORAGE_NOT_DIRECTORY,
          "regular file root is rejected", &failures);
    (void)unlink(path);
    check(join_path(path, sizeof path, workspace, "root-link") == 0 &&
              symlink(root, path) == 0 &&
              cn_storage_layout_init(&copied, path, &system_errno) ==
                  CN_STORAGE_SYMLINK,
          "final root symlink is rejected", &failures);
    (void)unlink(path);

    memcpy(mutable_root, root, strlen(root) + 1);
    check(cn_storage_layout_init(&copied, mutable_root, &system_errno) ==
              CN_STORAGE_OK,
          "caller buffer initializes", &failures);
    memset(mutable_root, 'x', strlen(mutable_root));
    check(strcmp(copied.root, root) == 0,
          "layout retains no caller root pointer", &failures);
    check(cn_storage_layout_init(&copied, root_two, &system_errno) ==
              CN_STORAGE_OK && strcmp(copied.root, root_two) == 0,
          "reinitialization replaces prior root", &failures);

    check(write_file(external, "", 0) != 0,
          "directory is not accepted as outside sentinel", &failures);
    check(join_path(path, sizeof path, workspace, "outside-sentinel") == 0 &&
              write_file(path, "outside\n", 1) == 0,
          "outside sentinel created", &failures);
    check(join_path(mutable_root, sizeof mutable_root, path, "child") == 0 &&
              cn_storage_layout_init(&copied, mutable_root, &system_errno) ==
                  CN_STORAGE_NOT_DIRECTORY && system_errno == ENOTDIR,
          "non-directory ancestor is distinguished", &failures);
    check(cn_storage_layout_prepare(&layout, &system_errno) == CN_STORAGE_OK &&
              system_errno == 0,
          "fresh prepare succeeds", &failures);
    check(cn_storage_layout_prepare(&layout, &system_errno) == CN_STORAGE_OK,
          "second prepare is idempotent", &failures);
    check(is_real_directory(config) && is_real_directory(progress),
          "required directories are real directories", &failures);
    check(join_path(mutable_root, sizeof mutable_root, root, "state") == 0 &&
              has_mode(mutable_root, 0700) && has_mode(progress, 0700) &&
              has_mode(config, 0700),
          "new directories request mode 0700", &failures);
    check(chmod(config, 0755) == 0,
          "existing-directory mode fixture changes", &failures);
    check(cn_storage_layout_prepare(&layout, &system_errno) == CN_STORAGE_OK &&
              has_mode(config, 0755),
          "prepare does not chmod existing directories", &failures);
    check(file_equals(path, "outside\n"),
          "prepare leaves outside sentinel unchanged", &failures);
    check(cn_progress_store_open(&store, progress) == CN_PROGRESS_OK &&
              store != NULL,
          "derived PROGRESS satisfies ProgressStore contract", &failures);
    cn_progress_store_close(store);
    store = NULL;

    check(cn_storage_layout_path(&layout, (cn_storage_location)-1,
                                 exact, sizeof exact) == CN_STORAGE_INVALID &&
              exact[0] == '\0' &&
              cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_COUNT,
                                     exact, sizeof exact) ==
                  CN_STORAGE_INVALID && exact[0] == '\0',
          "location enum bounds fail closed", &failures);
    copied = layout;
    copied.root_length++;
    check(cn_storage_layout_path(&copied, CN_STORAGE_LOCATION_CONFIG,
                                 exact, sizeof exact) == CN_STORAGE_INVALID &&
              exact[0] == '\0' &&
              cn_storage_layout_prepare(&copied, &system_errno) ==
                  CN_STORAGE_INVALID,
          "malformed internal layout fails closed", &failures);
    check(strcmp(cn_storage_result_name((cn_storage_result)-1), "unknown") ==
              0 &&
              strcmp(cn_storage_result_name(CN_STORAGE_RESULT_COUNT),
                     "unknown") == 0,
          "status name bounds fail closed", &failures);

    /* Conflict fixtures use independent roots because prepare is partial. */
    if (join_path(path, sizeof path, workspace, "state-file-root") == 0 &&
        make_directory(path, 0700) == 0) {
        char child[CN_STORAGE_PATH_CAPACITY];
        join_path(child, sizeof child, path, "state");
        write_file(child, "file\n", 1);
        cn_storage_layout_init(&copied, path, &system_errno);
        check(cn_storage_layout_prepare(&copied, &system_errno) ==
                  CN_STORAGE_NOT_DIRECTORY,
              "state file conflict is rejected", &failures);
        unlink(child); rmdir(path);
    }
    if (join_path(path, sizeof path, workspace, "progress-file-root") == 0 &&
        make_directory(path, 0700) == 0) {
        char state[CN_STORAGE_PATH_CAPACITY];
        char child[CN_STORAGE_PATH_CAPACITY];
        join_path(state, sizeof state, path, "state");
        make_directory(state, 0700);
        join_path(child, sizeof child, state, "progress");
        write_file(child, "file\n", 1);
        cn_storage_layout_init(&copied, path, &system_errno);
        check(cn_storage_layout_prepare(&copied, &system_errno) ==
                  CN_STORAGE_NOT_DIRECTORY,
              "progress file conflict is rejected", &failures);
        unlink(child); rmdir(state); rmdir(path);
    }
    if (join_path(path, sizeof path, workspace, "config-file-root") == 0 &&
        make_directory(path, 0700) == 0) {
        char state[CN_STORAGE_PATH_CAPACITY];
        char progress_dir[CN_STORAGE_PATH_CAPACITY];
        char child[CN_STORAGE_PATH_CAPACITY];
        join_path(state, sizeof state, path, "state");
        make_directory(state, 0700);
        join_path(progress_dir, sizeof progress_dir, state, "progress");
        make_directory(progress_dir, 0700);
        join_path(child, sizeof child, path, "config");
        write_file(child, "file\n", 1);
        cn_storage_layout_init(&copied, path, &system_errno);
        check(cn_storage_layout_prepare(&copied, &system_errno) ==
                  CN_STORAGE_NOT_DIRECTORY,
              "config file conflict is rejected", &failures);
        unlink(child); rmdir(progress_dir); rmdir(state); rmdir(path);
    }
    if (join_path(path, sizeof path, workspace, "symlink-child-root") == 0 &&
        make_directory(path, 0700) == 0) {
        char child[CN_STORAGE_PATH_CAPACITY];
        join_path(child, sizeof child, path, "state");
        symlink(external, child);
        cn_storage_layout_init(&copied, path, &system_errno);
        check(cn_storage_layout_prepare(&copied, &system_errno) ==
                  CN_STORAGE_SYMLINK,
              "managed child symlink is rejected", &failures);
        unlink(child); rmdir(path);
    }

    if (cn_storage_layout_init(&copied, "/sys", &system_errno) ==
        CN_STORAGE_OK) {
        result = cn_storage_layout_prepare(&copied, &system_errno);
        check(result == CN_STORAGE_READ_ONLY ||
                  result == CN_STORAGE_PERMISSION_DENIED,
              "read-only or protected root is distinguished", &failures);
    } else {
        printf("[SKIP] /sys is unavailable for read-only probing\n");
    }

    unlink(path);
    printf("STORAGE LAYOUT API SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_physical(const char *root, const char *outside_sentinel)
{
    static const char sentinel_text[] = "crossnook-storage-sentinel\n";
    cn_storage_layout layout;
    cn_storage_layout trailing_layout;
    char trailing[CN_STORAGE_PATH_CAPACITY];
    char config[CN_STORAGE_PATH_CAPACITY];
    char progress[CN_STORAGE_PATH_CAPACITY];
    char sentinel[CN_STORAGE_PATH_CAPACITY];
    cn_progress_store *store = NULL;
    int system_errno;
    int failures = 0;

    if (!root || !outside_sentinel ||
        snprintf(trailing, sizeof trailing, "%s/", root) >=
            (int)sizeof trailing)
        return 2;
    check(cn_storage_layout_init(&layout, root, &system_errno) ==
              CN_STORAGE_OK,
          "physical root initializes", &failures);
    check(cn_storage_layout_init(&trailing_layout, trailing, &system_errno) ==
              CN_STORAGE_OK && strcmp(layout.root, trailing_layout.root) == 0,
          "physical trailing slash normalizes", &failures);
    check(cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG,
                                 config, sizeof config) == CN_STORAGE_OK,
          "physical CONFIG derives", &failures);
    check(cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                                 progress, sizeof progress) == CN_STORAGE_OK,
          "physical PROGRESS derives", &failures);
    check(cn_storage_layout_prepare(&layout, &system_errno) == CN_STORAGE_OK,
          "physical prepare succeeds", &failures);
    check(cn_storage_layout_prepare(&layout, &system_errno) == CN_STORAGE_OK,
          "physical repeated prepare succeeds", &failures);
    check(is_real_directory(config) && is_real_directory(progress),
          "physical directories exist", &failures);
    check(join_path(sentinel, sizeof sentinel, progress,
                    "storage-layout.sentinel") == 0 &&
              write_file(sentinel, sentinel_text, 1) == 0 &&
              file_equals(sentinel, sentinel_text) && unlink(sentinel) == 0,
          "physical progress sentinel round-trips", &failures);
    check(cn_progress_store_open(&store, progress) == CN_PROGRESS_OK &&
              store != NULL,
          "physical ProgressStore opens", &failures);
    cn_progress_store_close(store);
    check(file_equals(outside_sentinel, "outside\n"),
          "physical outside sentinel remains unchanged", &failures);

    printf("STORAGE root=%s\n", layout.root);
    printf("config=%s\n", config);
    printf("progress=%s\n", progress);
    printf("prepare=%s\n", failures == 0 ? "ok" : "failed");
    printf("repeat=%s\n", failures == 0 ? "ok" : "failed");
    printf("isolation=%s\n", failures == 0 ? "ok" : "failed");
    printf("STORAGE LAYOUT SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int run_expect_prepare(const char *root, const char *expected_name)
{
    cn_storage_layout layout;
    cn_storage_result result;
    int system_errno;
    if (cn_storage_layout_init(&layout, root, &system_errno) != CN_STORAGE_OK)
        return 1;
    result = cn_storage_layout_prepare(&layout, &system_errno);
    printf("STORAGE PREPARE result=%s errno=%d\n",
           cn_storage_result_name(result), system_errno);
    return strcmp(cn_storage_result_name(result), expected_name) == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--smoke") == 0)
        return run_smoke(argv[2]);
    if (argc == 4 && strcmp(argv[1], "--physical") == 0)
        return run_physical(argv[2], argv[3]);
    if (argc == 4 && strcmp(argv[1], "--expect-prepare") == 0)
        return run_expect_prepare(argv[2], argv[3]);
    fprintf(stderr,
            "usage: crossnook-storage-layout-test --smoke <workspace>\n"
            "       crossnook-storage-layout-test --physical "
            "<existing-root> <outside-sentinel>\n"
            "       crossnook-storage-layout-test --expect-prepare "
            "<existing-root> <result-name>\n");
    return 2;
}
