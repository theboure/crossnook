/* Private RFC 1321 MD5 primitive for KOReader compatibility identities. */
#ifndef CN_BOOK_MD5_H
#define CN_BOOK_MD5_H

#include <stddef.h>
#include <stdint.h>

typedef struct cn_md5_context {
    uint32_t state[4];
    uint64_t byte_count;
    unsigned char buffer[64];
    size_t buffer_length;
} cn_md5_context;

void cn_md5_init(cn_md5_context *context);
void cn_md5_update(cn_md5_context *context, const void *data, size_t length);
void cn_md5_final(cn_md5_context *context, unsigned char digest[16]);

#endif /* CN_BOOK_MD5_H */
