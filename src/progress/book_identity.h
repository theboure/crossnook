/*
 * book_identity.h - opaque document identity boundary for local progress.
 *
 * The initial implementation derives a deterministic token from the exact
 * path bytes used to open a book. Callers may compare or log the token but
 * must not infer document metadata from it. A future content/KOSync identity
 * scheme can replace this implementation without changing ReaderPosition or
 * ProgressStore callers.
 */
#ifndef CN_PROGRESS_BOOK_IDENTITY_H
#define CN_PROGRESS_BOOK_IDENTITY_H

#define CN_BOOK_IDENTITY_TOKEN_BYTES 25
#define CN_BOOK_IDENTITY_PATH_MAX_BYTES 4096

typedef struct cn_book_identity {
    char token[CN_BOOK_IDENTITY_TOKEN_BYTES];
} cn_book_identity;

void cn_book_identity_init(cn_book_identity *identity);

/* Derive an opaque identity from the exact path bytes supplied to Reader.
 * Paths need not be UTF-8. Returns 0 on success and -1 for empty/overlong
 * paths or invalid arguments. */
int cn_book_identity_from_path(cn_book_identity *identity, const char *path);

/* Returns NULL for an invalid/uninitialized identity. */
const char *cn_book_identity_token(const cn_book_identity *identity);

#endif /* CN_PROGRESS_BOOK_IDENTITY_H */
