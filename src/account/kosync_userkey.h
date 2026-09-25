/* KOReader-compatible KOSync password-to-userkey derivation. */
#ifndef CN_ACCOUNT_KOSYNC_USERKEY_H
#define CN_ACCOUNT_KOSYNC_USERKEY_H

#include <stddef.h>

#define CN_ACCOUNT_PASSWORD_MAX 1024
#define CN_ACCOUNT_USERKEY_BYTES 16
#define CN_ACCOUNT_USERKEY_TEXT_LENGTH 32
#define CN_ACCOUNT_USERKEY_TEXT_CAPACITY (CN_ACCOUNT_USERKEY_TEXT_LENGTH + 1)

typedef enum cn_kosync_userkey_result {
    CN_KOSYNC_USERKEY_OK = 0,
    CN_KOSYNC_USERKEY_INVALID_ARGUMENT,
    CN_KOSYNC_USERKEY_PASSWORD_EMPTY,
    CN_KOSYNC_USERKEY_PASSWORD_TOO_LONG,
    CN_KOSYNC_USERKEY_OUTPUT_TOO_SMALL,
    CN_KOSYNC_USERKEY_RESULT_COUNT
} cn_kosync_userkey_result;

/* The password buffer is caller-owned and is never modified. */
cn_kosync_userkey_result cn_kosync_userkey_from_password(
    const unsigned char *password,
    size_t password_length,
    char *output,
    size_t output_capacity);

const char *cn_kosync_userkey_result_name(cn_kosync_userkey_result result);

#endif /* CN_ACCOUNT_KOSYNC_USERKEY_H */
