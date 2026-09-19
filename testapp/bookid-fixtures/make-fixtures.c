/* Generate sparse deterministic files around every KOReader sample offset. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define SAMPLE_BYTES 1024

static const uint64_t SAMPLE_OFFSETS[] = {
    UINT64_C(0), UINT64_C(1024), UINT64_C(4096), UINT64_C(16384),
    UINT64_C(65536), UINT64_C(262144), UINT64_C(1048576),
    UINT64_C(4194304), UINT64_C(16777216), UINT64_C(67108864),
    UINT64_C(268435456), UINT64_C(1073741824)
};

static unsigned char pattern_byte(uint64_t offset)
{
    return (unsigned char)((offset * UINT64_C(131) + 17) & 0xff);
}

static int write_fixture(const char *directory, const char *name,
                         uint64_t size, int change_kind)
{
    unsigned char sample[SAMPLE_BYTES];
    char path[4096];
    FILE *file;
    size_t i;
    if (snprintf(path, sizeof path, "%s/%s", directory, name) >=
        (int)sizeof path)
        return -1;
    file = fopen(path, "wb");
    if (!file)
        return -1;
    if (size != 0) {
        if (fseeko(file, (off_t)(size - 1), SEEK_SET) != 0 ||
            fputc(0, file) == EOF)
            goto error;
    }
    for (i = 0; i < sizeof SAMPLE_OFFSETS / sizeof SAMPLE_OFFSETS[0]; ++i) {
        uint64_t offset = SAMPLE_OFFSETS[i];
        size_t count;
        size_t j;
        if (offset >= size)
            break;
        count = size - offset < SAMPLE_BYTES
              ? (size_t)(size - offset) : SAMPLE_BYTES;
        for (j = 0; j < count; ++j)
            sample[j] = pattern_byte(offset + j);
        if (fseeko(file, (off_t)offset, SEEK_SET) != 0 ||
            fwrite(sample, 1, count, file) != count)
            goto error;
    }
    if (change_kind != 0) {
        uint64_t offset = change_kind == 1 ? UINT64_C(3000)
                                           : UINT64_C(4100);
        if (offset >= size || fseeko(file, (off_t)offset, SEEK_SET) != 0 ||
            fputc(change_kind == 1 ? 0x7f : 0x55, file) == EOF)
            goto error;
    }
    return fclose(file) == 0 ? 0 : -1;

error:
    fclose(file);
    return -1;
}

static int boundary_fixture(const char *directory, uint64_t size)
{
    char name[64];
    snprintf(name, sizeof name, "eof-%" PRIu64 ".bin", size);
    return write_fixture(directory, name, size, 0);
}

int main(int argc, char **argv)
{
    static const uint64_t SMALL_SIZES[] = {
        UINT64_C(0), UINT64_C(1), UINT64_C(1023), UINT64_C(1024),
        UINT64_C(1025), UINT64_C(2048), UINT64_C(2049)
    };
    const char *directory;
    size_t i;
    if (argc != 2) {
        fprintf(stderr, "usage: make-fixtures <output-directory>\n");
        return 2;
    }
    directory = argv[1];
    for (i = 0; i < sizeof SMALL_SIZES / sizeof SMALL_SIZES[0]; ++i) {
        if (boundary_fixture(directory, SMALL_SIZES[i]) != 0)
            goto error;
    }
    for (i = 2; i < sizeof SAMPLE_OFFSETS / sizeof SAMPLE_OFFSETS[0]; ++i) {
        if (boundary_fixture(directory, SAMPLE_OFFSETS[i]) != 0 ||
            boundary_fixture(directory, SAMPLE_OFFSETS[i] + 1) != 0)
            goto error;
    }
    if (write_fixture(directory, "all-samples.bin",
                      SAMPLE_OFFSETS[11] + SAMPLE_BYTES, 0) != 0 ||
        write_fixture(directory, "gap-base.bin", UINT64_C(6000), 0) != 0 ||
        write_fixture(directory, "gap-changed.bin", UINT64_C(6000), 1) != 0 ||
        write_fixture(directory, "sample-changed.bin", UINT64_C(6000), 2) != 0)
        goto error;
    return 0;

error:
    fprintf(stderr, "fixture generation failed: %s\n", strerror(errno));
    return 1;
}
