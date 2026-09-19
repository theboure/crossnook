#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "progress/book_identity.h"

static const char ID_PREFIX[] = "path-v1-";

void cn_book_identity_init(cn_book_identity *identity)
{
    if (identity)
        memset(identity, 0, sizeof *identity);
}

static int identity_valid(const cn_book_identity *identity)
{
    size_t i;
    if (!identity || memcmp(identity->token, ID_PREFIX,
                            sizeof ID_PREFIX - 1) != 0)
        return 0;
    for (i = sizeof ID_PREFIX - 1; i < CN_BOOK_IDENTITY_TOKEN_BYTES - 1; ++i) {
        if (!((identity->token[i] >= '0' && identity->token[i] <= '9') ||
              (identity->token[i] >= 'a' && identity->token[i] <= 'f')))
            return 0;
    }
    return identity->token[CN_BOOK_IDENTITY_TOKEN_BYTES - 1] == '\0';
}

int cn_book_identity_from_path(cn_book_identity *identity, const char *path)
{
    static const char hex[] = "0123456789abcdef";
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t length;
    size_t i;
    if (!identity)
        return -1;
    cn_book_identity_init(identity);
    if (!path)
        return -1;
    for (length = 0; length <= CN_BOOK_IDENTITY_PATH_MAX_BYTES; ++length) {
        if (path[length] == '\0')
            break;
    }
    if (length == 0 || length > CN_BOOK_IDENTITY_PATH_MAX_BYTES)
        return -1;
    for (i = 0; i < length; ++i) {
        hash ^= (unsigned char)path[i];
        hash *= UINT64_C(1099511628211);
    }
    memcpy(identity->token, ID_PREFIX, sizeof ID_PREFIX - 1);
    for (i = 0; i < 16; ++i) {
        unsigned int shift = (unsigned int)((15 - i) * 4);
        identity->token[sizeof ID_PREFIX - 1 + i] =
            hex[(hash >> shift) & 0x0f];
    }
    identity->token[CN_BOOK_IDENTITY_TOKEN_BYTES - 1] = '\0';
    return 0;
}

const char *cn_book_identity_token(const cn_book_identity *identity)
{
    return identity_valid(identity) ? identity->token : NULL;
}
