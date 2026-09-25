#include <stdint.h>
#include <string.h>

#include "account/kosync_userkey.h"
#include "book/md5.h"

static void clear_bytes(void *data, size_t length)
{
    volatile unsigned char *bytes = (volatile unsigned char *)data;
    while (length--)
        *bytes++ = 0;
}

cn_kosync_userkey_result cn_kosync_userkey_from_password(
    const unsigned char *password, size_t password_length, char *output,
    size_t output_capacity)
{
    static const char hex[] = "0123456789abcdef";
    cn_md5_context context;
    unsigned char digest[CN_ACCOUNT_USERKEY_BYTES];
    cn_kosync_userkey_result result = CN_KOSYNC_USERKEY_OK;
    size_t i;

    if (output && output_capacity > 0)
        output[0] = '\0';
    memset(&context, 0, sizeof context);
    memset(digest, 0, sizeof digest);
    if (!password || !output)
        result = CN_KOSYNC_USERKEY_INVALID_ARGUMENT;
    else if (password_length == 0)
        result = CN_KOSYNC_USERKEY_PASSWORD_EMPTY;
    else if (password_length > CN_ACCOUNT_PASSWORD_MAX)
        result = CN_KOSYNC_USERKEY_PASSWORD_TOO_LONG;
    else if (output_capacity < CN_ACCOUNT_USERKEY_TEXT_CAPACITY)
        result = CN_KOSYNC_USERKEY_OUTPUT_TOO_SMALL;
    if (result != CN_KOSYNC_USERKEY_OK)
        goto done;

    cn_md5_init(&context);
    cn_md5_update(&context, password, password_length);
    cn_md5_final(&context, digest);
    for (i = 0; i < sizeof digest; ++i) {
        output[i * 2] = hex[digest[i] >> 4];
        output[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    output[CN_ACCOUNT_USERKEY_TEXT_LENGTH] = '\0';

done:
    clear_bytes(&context, sizeof context);
    clear_bytes(digest, sizeof digest);
    if (result != CN_KOSYNC_USERKEY_OK && output && output_capacity > 0)
        output[0] = '\0';
    return result;
}

const char *cn_kosync_userkey_result_name(cn_kosync_userkey_result result)
{
    static const char *const names[CN_KOSYNC_USERKEY_RESULT_COUNT] = {
        "ok", "invalid-argument", "password-empty", "password-too-long",
        "output-too-small"
    };
    return result >= 0 && result < CN_KOSYNC_USERKEY_RESULT_COUNT
               ? names[result] : "unknown";
}
