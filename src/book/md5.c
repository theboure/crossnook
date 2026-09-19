#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book/md5.h"

static uint32_t rotate_left(uint32_t value, unsigned int shift)
{
    return (value << shift) | (value >> (32 - shift));
}

static uint32_t read_u32le(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void write_u32le(unsigned char *p, uint32_t value)
{
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
    p[2] = (unsigned char)(value >> 16);
    p[3] = (unsigned char)(value >> 24);
}

static void transform(cn_md5_context *context, const unsigned char block[64])
{
    static const uint32_t constants[64] = {
        UINT32_C(0xd76aa478), UINT32_C(0xe8c7b756), UINT32_C(0x242070db), UINT32_C(0xc1bdceee),
        UINT32_C(0xf57c0faf), UINT32_C(0x4787c62a), UINT32_C(0xa8304613), UINT32_C(0xfd469501),
        UINT32_C(0x698098d8), UINT32_C(0x8b44f7af), UINT32_C(0xffff5bb1), UINT32_C(0x895cd7be),
        UINT32_C(0x6b901122), UINT32_C(0xfd987193), UINT32_C(0xa679438e), UINT32_C(0x49b40821),
        UINT32_C(0xf61e2562), UINT32_C(0xc040b340), UINT32_C(0x265e5a51), UINT32_C(0xe9b6c7aa),
        UINT32_C(0xd62f105d), UINT32_C(0x02441453), UINT32_C(0xd8a1e681), UINT32_C(0xe7d3fbc8),
        UINT32_C(0x21e1cde6), UINT32_C(0xc33707d6), UINT32_C(0xf4d50d87), UINT32_C(0x455a14ed),
        UINT32_C(0xa9e3e905), UINT32_C(0xfcefa3f8), UINT32_C(0x676f02d9), UINT32_C(0x8d2a4c8a),
        UINT32_C(0xfffa3942), UINT32_C(0x8771f681), UINT32_C(0x6d9d6122), UINT32_C(0xfde5380c),
        UINT32_C(0xa4beea44), UINT32_C(0x4bdecfa9), UINT32_C(0xf6bb4b60), UINT32_C(0xbebfbc70),
        UINT32_C(0x289b7ec6), UINT32_C(0xeaa127fa), UINT32_C(0xd4ef3085), UINT32_C(0x04881d05),
        UINT32_C(0xd9d4d039), UINT32_C(0xe6db99e5), UINT32_C(0x1fa27cf8), UINT32_C(0xc4ac5665),
        UINT32_C(0xf4292244), UINT32_C(0x432aff97), UINT32_C(0xab9423a7), UINT32_C(0xfc93a039),
        UINT32_C(0x655b59c3), UINT32_C(0x8f0ccc92), UINT32_C(0xffeff47d), UINT32_C(0x85845dd1),
        UINT32_C(0x6fa87e4f), UINT32_C(0xfe2ce6e0), UINT32_C(0xa3014314), UINT32_C(0x4e0811a1),
        UINT32_C(0xf7537e82), UINT32_C(0xbd3af235), UINT32_C(0x2ad7d2bb), UINT32_C(0xeb86d391)
    };
    static const unsigned char shifts[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
    };
    uint32_t words[16];
    uint32_t a = context->state[0];
    uint32_t b = context->state[1];
    uint32_t c = context->state[2];
    uint32_t d = context->state[3];
    unsigned int i;

    for (i = 0; i < 16; ++i)
        words[i] = read_u32le(block + i * 4);
    for (i = 0; i < 64; ++i) {
        uint32_t function;
        unsigned int word;
        uint32_t previous_d = d;
        if (i < 16) {
            function = (b & c) | (~b & d);
            word = i;
        } else if (i < 32) {
            function = (d & b) | (~d & c);
            word = (5 * i + 1) & 15;
        } else if (i < 48) {
            function = b ^ c ^ d;
            word = (3 * i + 5) & 15;
        } else {
            function = c ^ (b | ~d);
            word = (7 * i) & 15;
        }
        d = c;
        c = b;
        b += rotate_left(a + function + constants[i] + words[word], shifts[i]);
        a = previous_d;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
}

void cn_md5_init(cn_md5_context *context)
{
    context->state[0] = UINT32_C(0x67452301);
    context->state[1] = UINT32_C(0xefcdab89);
    context->state[2] = UINT32_C(0x98badcfe);
    context->state[3] = UINT32_C(0x10325476);
    context->byte_count = 0;
    context->buffer_length = 0;
}

void cn_md5_update(cn_md5_context *context, const void *input, size_t length)
{
    const unsigned char *data = (const unsigned char *)input;
    context->byte_count += length;
    if (context->buffer_length != 0) {
        size_t needed = 64 - context->buffer_length;
        if (needed > length)
            needed = length;
        memcpy(context->buffer + context->buffer_length, data, needed);
        context->buffer_length += needed;
        data += needed;
        length -= needed;
        if (context->buffer_length == 64) {
            transform(context, context->buffer);
            context->buffer_length = 0;
        }
    }
    while (length >= 64) {
        transform(context, data);
        data += 64;
        length -= 64;
    }
    if (length != 0) {
        memcpy(context->buffer, data, length);
        context->buffer_length = length;
    }
}

void cn_md5_final(cn_md5_context *context, unsigned char digest[16])
{
    static const unsigned char padding[64] = { 0x80 };
    unsigned char length_bytes[8];
    uint64_t bit_count = context->byte_count * 8;
    size_t padding_length = context->buffer_length < 56
                          ? 56 - context->buffer_length
                          : 120 - context->buffer_length;
    unsigned int i;

    for (i = 0; i < 8; ++i)
        length_bytes[i] = (unsigned char)(bit_count >> (i * 8));
    cn_md5_update(context, padding, padding_length);
    cn_md5_update(context, length_bytes, sizeof length_bytes);
    for (i = 0; i < 4; ++i)
        write_u32le(digest + i * 4, context->state[i]);
}
