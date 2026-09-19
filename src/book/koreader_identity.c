#include <stdio.h>
#include <string.h>

#include "book/koreader_identity.h"
#include "book/md5.h"

#define KOREADER_SAMPLE_BYTES 1024

static const long SAMPLE_OFFSETS[] = {
    0L, 1024L, 4096L, 16384L, 65536L, 262144L,
    1048576L, 4194304L, 16777216L, 67108864L,
    268435456L, 1073741824L
};

static int bounded_length(const char *text, size_t *length)
{
    size_t n;
    if (!text)
        return -1;
    for (n = 0; n <= CN_KOREADER_IDENTITY_PATH_MAX_BYTES; ++n) {
        if (text[n] == '\0') {
            *length = n;
            return 0;
        }
    }
    return -1;
}

static void digest_to_text(const unsigned char digest[16],
                           cn_koreader_document_id *identity)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 16; ++i) {
        identity->hex[i * 2] = hex[digest[i] >> 4];
        identity->hex[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    identity->hex[32] = '\0';
}

static void md5_text(const void *data, size_t length,
                     cn_koreader_document_id *identity)
{
    cn_md5_context context;
    unsigned char digest[16];
    cn_md5_init(&context);
    cn_md5_update(&context, data, length);
    cn_md5_final(&context, digest);
    digest_to_text(digest, identity);
}

void cn_koreader_document_id_init(cn_koreader_document_id *identity)
{
    if (identity)
        memset(identity, 0, sizeof *identity);
}

cn_koreader_identity_result cn_book_identity_koreader_binary(
    const char *path, cn_koreader_document_id *identity)
{
    cn_md5_context context;
    unsigned char sample[KOREADER_SAMPLE_BYTES];
    unsigned char digest[16];
    FILE *file;
    size_t path_length;
    size_t i;

    if (!identity)
        return CN_KOREADER_IDENTITY_INVALID;
    cn_koreader_document_id_init(identity);
    if (bounded_length(path, &path_length) != 0 || path_length == 0)
        return CN_KOREADER_IDENTITY_INVALID;
    file = fopen(path, "rb");
    if (!file)
        return CN_KOREADER_IDENTITY_IO_ERROR;

    cn_md5_init(&context);
    for (i = 0; i < sizeof SAMPLE_OFFSETS / sizeof SAMPLE_OFFSETS[0]; ++i) {
        size_t count;
        if (fseek(file, SAMPLE_OFFSETS[i], SEEK_SET) != 0) {
            fclose(file);
            return CN_KOREADER_IDENTITY_IO_ERROR;
        }
        count = fread(sample, 1, sizeof sample, file);
        if (ferror(file)) {
            fclose(file);
            return CN_KOREADER_IDENTITY_IO_ERROR;
        }
        if (count == 0)
            break;
        cn_md5_update(&context, sample, count);
    }
    if (fclose(file) != 0)
        return CN_KOREADER_IDENTITY_IO_ERROR;
    cn_md5_final(&context, digest);
    digest_to_text(digest, identity);
    return CN_KOREADER_IDENTITY_OK;
}

cn_koreader_identity_result cn_book_identity_koreader_filename(
    const char *path, cn_koreader_document_id *identity)
{
    const char *name;
    size_t path_length;
    if (!identity)
        return CN_KOREADER_IDENTITY_INVALID;
    cn_koreader_document_id_init(identity);
    if (bounded_length(path, &path_length) != 0)
        return CN_KOREADER_IDENTITY_INVALID;
    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    md5_text(name, path_length - (size_t)(name - path), identity);
    return CN_KOREADER_IDENTITY_OK;
}

const char *cn_koreader_document_id_text(
    const cn_koreader_document_id *identity)
{
    size_t i;
    if (!identity || identity->hex[32] != '\0')
        return NULL;
    for (i = 0; i < 32; ++i) {
        if (!((identity->hex[i] >= '0' && identity->hex[i] <= '9') ||
              (identity->hex[i] >= 'a' && identity->hex[i] <= 'f')))
            return NULL;
    }
    return identity->hex;
}

const char *cn_koreader_identity_result_name(
    cn_koreader_identity_result result)
{
    switch (result) {
    case CN_KOREADER_IDENTITY_OK:       return "ok";
    case CN_KOREADER_IDENTITY_INVALID:  return "invalid";
    case CN_KOREADER_IDENTITY_IO_ERROR: return "io-error";
    default:                            return "unknown";
    }
}
