/* Synthetic mount/device tests and read-only physical verifier diagnostic. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "platform/storage_verify.h"
#include "settings/settings_store.h"
#include "storage/storage_layout.h"
#include "progress/progress_store.h"

typedef struct fixture {
    const char *mounts;
    const char *point;
    const char *root;
    int missing_root;
    int regular_root;
    int symlink;
    int wrong_root_device;
    int source_not_block;
    int source_wrong_device;
} fixture;

static int failures;

static void check(int pass, const char *name)
{
    printf("[%s] %s\n", pass ? "OK" : "FAIL", name);
    if (!pass)
        ++failures;
}

static int read_fixture(void *context, char *out, size_t capacity,
                        size_t *length)
{
    const fixture *f = context;
    *length = strlen(f->mounts);
    if (*length > capacity) {
        errno = EOVERFLOW;
        return -1;
    }
    memcpy(out, f->mounts, *length);
    return 0;
}

static int prefix(const char *path, const char *parent)
{
    size_t n = strlen(parent);
    return strncmp(path, parent, n) == 0 &&
           (path[n] == '/' || path[n] == 0);
}

static int normalized_equal(const char *actual, const char *supplied)
{
    size_t length = strlen(supplied);
    while (length > 1 && supplied[length - 1] == '/')
        --length;
    return strlen(actual) == length && memcmp(actual, supplied, length) == 0;
}

static int fixture_lstat(void *context, const char *path, struct stat *st)
{
    fixture *f = context;
    memset(st, 0, sizeof *st);
    st->st_dev = makedev(0, 1);
    st->st_mode = S_IFDIR | 0700;
    if (strcmp(path, "/") == 0 || strcmp(path, "/tmp") == 0 ||
        strcmp(path, "/sdcard") == 0)
        return 0;
    if (prefix(path, f->point)) {
        st->st_dev = makedev(179, 16);
        if (strcmp(path, f->root) == 0) {
            if (f->missing_root) {
                errno = ENOENT;
                return -1;
            }
            if (f->regular_root)
                st->st_mode = S_IFREG | 0600;
            if (f->wrong_root_device)
                st->st_dev = makedev(0, 1);
        }
        if (f->symlink && strcmp(path, f->point) != 0)
            st->st_mode = S_IFLNK | 0777;
        return 0;
    }
    errno = ENOENT;
    return -1;
}

static int fixture_stat(void *context, const char *path, struct stat *st)
{
    fixture *f = context;
    if (strcmp(path, "/dev/block/mmcblk1") != 0 &&
        strcmp(path, "/dev/block/mmcblk0") != 0) {
        errno = ENOENT;
        return -1;
    }
    memset(st, 0, sizeof *st);
    st->st_mode = f->source_not_block ? S_IFREG : S_IFBLK;
    st->st_rdev = f->source_wrong_device ||
                  strcmp(path, "/dev/block/mmcblk0") == 0
                      ? makedev(179, 0) : makedev(179, 16);
    return 0;
}

static cn_platform_storage_result run_case(fixture *f, const char *root,
                                            const char *point,
                                            unsigned major_id,
                                            unsigned minor_id,
                                            int *zero)
{
    cn_platform_storage_candidate candidate = {root, point, major_id, minor_id};
    cn_platform_storage_verified verified;
    cn_platform_storage_source source = {
        f, read_fixture, fixture_lstat, fixture_stat
    };
    cn_platform_storage_result result;
    int system_errno = 123;
    memset(&verified, 0x5a, sizeof verified);
    result = cn_platform_storage_verify_with_source(&candidate, &source,
                                                     &verified, &system_errno);
    *zero = verified.root[0] == 0 && verified.mountpoint[0] == 0 &&
            verified.device_major == 0 && verified.device_minor == 0;
    if (result == CN_PLATFORM_STORAGE_OK)
        *zero = normalized_equal(verified.root, root) &&
                normalized_equal(verified.mountpoint, point) &&
                verified.device_major == major_id &&
                verified.device_minor == minor_id && system_errno == 0;
    return result;
}

#define ROOTFS "1 0 0:1 / / rw - rootfs rootfs rw\n"
#define CARD "20 1 179:16 / /tmp/crossnook-card rw - vfat /dev/block/mmcblk1 rw\n"

static int smoke(void)
{
    fixture f = {ROOTFS CARD, "/tmp/crossnook-card",
                 "/tmp/crossnook-card/crossnook", 0, 0, 0, 0, 0, 0};
    char oversized[CN_PLATFORM_STORAGE_PATH_CAPACITY + 1];
    int valid;
    cn_platform_storage_result r;
#define EXPECT(label, wanted, root, point, maj, min) do { \
    r = run_case(&f, root, point, maj, min, &valid); \
    check(r == wanted && valid, label); \
} while (0)
    EXPECT("whole-device source and child root", CN_PLATFORM_STORAGE_OK,
           f.root, f.point, 179, 16);
    EXPECT("candidate exactly at mounted filesystem", CN_PLATFORM_STORAGE_OK,
           f.point, f.point, 179, 16);
    EXPECT("component boundary rejects similar prefix", CN_PLATFORM_STORAGE_WRONG_MOUNT,
           "/tmp/crossnook-card-other/root", f.point, 179, 16);
    EXPECT("rootfs fallback is rejected", CN_PLATFORM_STORAGE_MOUNT_MISSING,
           "/sdcard", "/sdcard", 179, 16);
    f.mounts = ROOTFS;
    EXPECT("missing expected mount cannot become a rootfs write", CN_PLATFORM_STORAGE_MOUNT_MISSING,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS CARD
        "21 20 179:0 / /tmp/crossnook-card/nested rw - ext3 /dev/block/mmcblk0 rw\n";
    EXPECT("nested wrong filesystem wins longest prefix", CN_PLATFORM_STORAGE_WRONG_MOUNT,
           "/tmp/crossnook-card/nested/root", f.point, 179, 16);
    f.mounts = ROOTFS "20 1 179:16 / /tmp/crossnook-card ro - vfat /dev/block/mmcblk1 ro\n";
    EXPECT("read-only mount rejected", CN_PLATFORM_STORAGE_READ_ONLY,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS CARD;
    EXPECT("wrong expected major/minor rejected", CN_PLATFORM_STORAGE_WRONG_DEVICE,
           f.root, f.point, 179, 0);
    f.mounts = ROOTFS "20 1 179:16 / /tmp/crossnook-card rw - vfat /dev/block/mmcblk0 rw\n";
    EXPECT("wrong source device rejected", CN_PLATFORM_STORAGE_WRONG_DEVICE,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS CARD;
    f.source_wrong_device = 1;
    EXPECT("source rdev must match mount identity", CN_PLATFORM_STORAGE_WRONG_DEVICE,
           f.root, f.point, 179, 16);
    f.source_wrong_device = 0;
    f.source_not_block = 1;
    EXPECT("source must be block special", CN_PLATFORM_STORAGE_WRONG_DEVICE,
           f.root, f.point, 179, 16);
    f.source_not_block = 0;
    f.missing_root = 1;
    EXPECT("preprovisioned root required", CN_PLATFORM_STORAGE_ROOT_MISSING,
           f.root, f.point, 179, 16);
    f.missing_root = 0;
    f.regular_root = 1;
    EXPECT("root must be directory", CN_PLATFORM_STORAGE_NOT_DIRECTORY,
           f.root, f.point, 179, 16);
    f.regular_root = 0;
    f.symlink = 1;
    EXPECT("symlink root escape rejected", CN_PLATFORM_STORAGE_SYMLINK,
           f.root, f.point, 179, 16);
    f.root = "/tmp/crossnook-card/link/root";
    EXPECT("symlink ancestor escape rejected", CN_PLATFORM_STORAGE_SYMLINK,
           f.root, f.point, 179, 16);
    f.root = "/tmp/crossnook-card/crossnook";
    f.symlink = 0;
    f.wrong_root_device = 1;
    EXPECT("root st_dev must equal selected filesystem", CN_PLATFORM_STORAGE_WRONG_MOUNT,
           f.root, f.point, 179, 16);
    f.wrong_root_device = 0;
    f.mounts = ROOTFS CARD CARD;
    EXPECT("duplicate mountpoint rejected", CN_PLATFORM_STORAGE_MALFORMED_MOUNTS,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS "20 1 179:16 /other /tmp/crossnook-card rw - vfat /dev/block/mmcblk1 rw\n";
    EXPECT("bind subdirectory mount rejected", CN_PLATFORM_STORAGE_UNSUPPORTED,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS "20 1 179:16 / /tmp/crossnook-card rw - tmpfs /dev/block/mmcblk1 rw\n";
    EXPECT("pseudo filesystem rejected", CN_PLATFORM_STORAGE_UNSUPPORTED,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS "20 1 179:16 / /tmp/crossnook-card rw - vfat /dev/block/mmcblk1 ro\n";
    EXPECT("readonly superblock rejected", CN_PLATFORM_STORAGE_READ_ONLY,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS CARD "21 bad\n";
    EXPECT("malformed extra mount fails closed", CN_PLATFORM_STORAGE_MALFORMED_MOUNTS,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS "20 1 179:16 / /tmp/crossnook\\0 rw - vfat /dev/block/mmcblk1 rw\n";
    EXPECT("incomplete mount escape rejected", CN_PLATFORM_STORAGE_MALFORMED_MOUNTS,
           f.root, f.point, 179, 16);
    f.mounts = ROOTFS "20 1 179:16 / /tmp/crossnook-card rw - vfat /dev/block/mmcblk1 rw";
    EXPECT("truncated mount table rejected", CN_PLATFORM_STORAGE_MALFORMED_MOUNTS,
           f.root, f.point, 179, 16);
    f.point = "/tmp/crossnook card";
    f.root = "/tmp/crossnook card/crossnook";
    f.mounts = ROOTFS "20 1 179:16 / /tmp/crossnook\\040card rw - vfat /dev/block/mmcblk1 rw\n";
    EXPECT("escaped mountpoint decoded exactly", CN_PLATFORM_STORAGE_OK,
           f.root, f.point, 179, 16);
    f.point = "/tmp/crossnook-card";
    f.root = "/tmp/crossnook-card/crossnook";
    f.mounts = ROOTFS CARD;
    EXPECT("dot component rejected", CN_PLATFORM_STORAGE_INVALID,
           "/tmp/./crossnook-card/crossnook", f.point, 179, 16);
    EXPECT("relative root rejected", CN_PLATFORM_STORAGE_INVALID,
           "tmp/crossnook-card/crossnook", f.point, 179, 16);
    EXPECT("filesystem root cannot be selected", CN_PLATFORM_STORAGE_INVALID,
           "/", f.point, 179, 16);
    memset(oversized, 'x', sizeof oversized);
    oversized[0] = '/';
    oversized[CN_PLATFORM_STORAGE_PATH_CAPACITY] = 0;
    EXPECT("overlong root rejected without truncation", CN_PLATFORM_STORAGE_PATH_TOO_LONG,
           oversized, f.point, 179, 16);
    EXPECT("repeated separator rejected", CN_PLATFORM_STORAGE_INVALID,
           "/tmp//crossnook-card/crossnook", f.point, 179, 16);
    EXPECT("trailing root slash normalizes", CN_PLATFORM_STORAGE_OK,
           "/tmp/crossnook-card/crossnook/", f.point, 179, 16);
    EXPECT("no implicit diagnostic /tmp bypass", CN_PLATFORM_STORAGE_WRONG_MOUNT,
           "/tmp", f.point, 179, 16);
    check(strcmp(cn_platform_storage_result_name((cn_platform_storage_result)-1),
                 "unknown") == 0 &&
          strcmp(cn_platform_storage_result_name(CN_PLATFORM_STORAGE_RESULT_COUNT),
                 "unknown") == 0, "result names fail closed");
    printf("STORAGE VERIFY SMOKE failures=%d -> %s\n", failures,
           failures ? "FAIL" : "OK");
    return failures ? 1 : 0;
}

static int parse_number(const char *text, unsigned *out)
{
    char *end;
    unsigned long n;
    errno = 0;
    n = strtoul(text, &end, 10);
    if (errno || !*text || *end || n > 65535)
        return 0;
    *out = (unsigned)n;
    return 1;
}

static int physical(int argc, char **argv)
{
    cn_platform_storage_candidate candidate;
    cn_platform_storage_verified verified;
    cn_platform_storage_result result;
    int system_errno = 0;
    candidate.mountpoint = argv[2];
    candidate.root = argv[3];
    if (!parse_number(argv[4], &candidate.expected_major) ||
        !parse_number(argv[5], &candidate.expected_minor))
        return 2;
    result = cn_platform_storage_verify(&candidate, &verified, &system_errno);
    printf("STORAGE VERIFY result=%s errno=%d root=%s mount=%s device=%u:%u\n",
           cn_platform_storage_result_name(result), system_errno,
           verified.root, verified.mountpoint,
           verified.device_major, verified.device_minor);
    if (strcmp(argv[1], "--expect") == 0)
        return argc == 7 &&
               strcmp(cn_platform_storage_result_name(result), argv[6]) == 0
                   ? 0 : 1;
    if (result != CN_PLATFORM_STORAGE_OK)
        return 1;
    if (strcmp(argv[1], "--verify") == 0)
        return 0;
    if (strcmp(argv[1], "--compose") == 0) {
        cn_storage_layout layout;
        cn_settings_store store;
        cn_settings settings;
        cn_progress_store *progress_store = NULL;
        cn_settings_result settings_result;
        char config[CN_STORAGE_PATH_CAPACITY];
        char progress[CN_STORAGE_PATH_CAPACITY];
        if (cn_storage_layout_init(&layout, verified.root, &system_errno) != CN_STORAGE_OK ||
            cn_storage_layout_prepare(&layout, &system_errno) != CN_STORAGE_OK ||
            cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_CONFIG,
                                   config, sizeof config) != CN_STORAGE_OK ||
            cn_storage_layout_path(&layout, CN_STORAGE_LOCATION_PROGRESS,
                                   progress, sizeof progress) != CN_STORAGE_OK ||
            cn_settings_store_init(&store, config, &system_errno) != CN_SETTINGS_OK)
            return 1;
        settings_result = cn_settings_load(&store, &settings, &system_errno);
        if (settings_result != CN_SETTINGS_OK && settings_result != CN_SETTINGS_MISSING)
            return 1;
        if (cn_progress_store_open(&progress_store, progress) != CN_PROGRESS_OK)
            return 1;
        cn_progress_store_close(progress_store);
        printf("STORAGE COMPOSE config=%s progress=%s settings=%s -> OK\n",
               config, progress, cn_settings_result_name(settings_result));
        return 0;
    }
    return 2;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--smoke") == 0)
        return smoke();
    if ((argc == 6 && (strcmp(argv[1], "--verify") == 0 ||
                       strcmp(argv[1], "--compose") == 0)) ||
        (argc == 7 && strcmp(argv[1], "--expect") == 0))
        return physical(argc, argv);
    fprintf(stderr, "usage: storage-verify-test --smoke | --verify/--compose <mount> <root> <major> <minor> | --expect <mount> <root> <major> <minor> <result>\n");
    return 2;
}
