/* CLI and API smoke checks for KOReader-compatible document identity. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book/koreader_identity.h"
#include "progress/book_identity.h"

static void check(int condition, const char *name, int *failures)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        (*failures)++;
}

static int same_filename_id(const char *a, const char *b)
{
    cn_koreader_document_id first;
    cn_koreader_document_id second;
    return cn_book_identity_koreader_filename(a, &first) ==
               CN_KOREADER_IDENTITY_OK &&
           cn_book_identity_koreader_filename(b, &second) ==
               CN_KOREADER_IDENTITY_OK &&
           strcmp(first.hex, second.hex) == 0;
}

static int filename_id_is(const char *path, const char *expected)
{
    cn_koreader_document_id identity;
    return cn_book_identity_koreader_filename(path, &identity) ==
               CN_KOREADER_IDENTITY_OK &&
           strcmp(identity.hex, expected) == 0;
}

static int run_api_smoke(void)
{
    cn_koreader_document_id identity;
    cn_book_identity local;
    char padding_probe[66];
    char *oversized;
    int failures = 0;

    check(same_filename_id("/books/Book.EPUB", "/other/Book.EPUB"),
          "Filename mode excludes directory path", &failures);
    check(same_filename_id("Book.EPUB", "/books/Book.EPUB"),
          "Filename mode includes basename and extension", &failures);
    check(!same_filename_id("Book.EPUB", "book.epub"),
          "Filename mode preserves case", &failures);
    check(!same_filename_id("Book.EPUB", "Book"),
          "Filename mode preserves extension", &failures);
    check(!same_filename_id("C:\\books\\Book.EPUB", "Book.EPUB"),
          "Filename mode does not split backslashes", &failures);
    check(cn_book_identity_koreader_filename("", &identity) ==
              CN_KOREADER_IDENTITY_OK &&
          strcmp(identity.hex, "d41d8cd98f00b204e9800998ecf8427e") == 0 &&
          same_filename_id("", "/books/"),
          "Filename mode hashes an empty basename", &failures);
    memset(padding_probe, 'a', sizeof padding_probe);
    padding_probe[55] = '\0';
    check(filename_id_is(padding_probe,
                         "ef1772b6dff9a122358552954ad0df65"),
          "MD5 55-byte padding boundary matches pinned oracle", &failures);
    padding_probe[55] = 'a';
    padding_probe[56] = '\0';
    check(filename_id_is(padding_probe,
                         "3b0c8ac703f828b04c6c197006d17218"),
          "MD5 56-byte padding boundary matches pinned oracle", &failures);

    memset(&identity, 'x', sizeof identity);
    check(cn_book_identity_koreader_binary(NULL, &identity) ==
              CN_KOREADER_IDENTITY_INVALID && identity.hex[0] == '\0',
          "Binary mode rejects NULL and clears output", &failures);
    check(cn_book_identity_koreader_binary("", &identity) ==
              CN_KOREADER_IDENTITY_INVALID,
          "Binary mode rejects an empty path", &failures);
    oversized = (char *)malloc(CN_KOREADER_IDENTITY_PATH_MAX_BYTES + 2);
    if (oversized) {
        memset(oversized, 'a', CN_KOREADER_IDENTITY_PATH_MAX_BYTES + 1);
        oversized[CN_KOREADER_IDENTITY_PATH_MAX_BYTES + 1] = '\0';
    }
    check(oversized &&
          cn_book_identity_koreader_binary(oversized, &identity) ==
              CN_KOREADER_IDENTITY_INVALID &&
          cn_book_identity_koreader_filename(oversized, &identity) ==
              CN_KOREADER_IDENTITY_INVALID,
          "oversized paths are rejected", &failures);
    free(oversized);

    cn_book_identity_init(&local);
    check(cn_book_identity_from_path(&local, "books/error.epub") == 0 &&
          strcmp(cn_book_identity_token(&local),
                 "path-v1-9e285c67105cacd8") == 0,
          "Local Progress path-v1 identity is unchanged", &failures);

    printf("BOOKID API SMOKE failures=%d -> %s\n", failures,
           failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

static int print_identity(const char *mode, const char *path)
{
    cn_koreader_document_id identity;
    cn_koreader_identity_result result;
    if (strcmp(mode, "binary") == 0)
        result = cn_book_identity_koreader_binary(path, &identity);
    else
        result = cn_book_identity_koreader_filename(path, &identity);
    if (result != CN_KOREADER_IDENTITY_OK) {
        fprintf(stderr, "BOOKID %s ERROR %s\n", mode,
                cn_koreader_identity_result_name(result));
        return 1;
    }
    printf("BOOKID %s %s\n", mode,
           cn_koreader_document_id_text(&identity));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--api-smoke") == 0)
        return run_api_smoke();
    if (argc == 3 && strcmp(argv[1], "--binary") == 0)
        return print_identity("binary", argv[2]);
    if (argc == 3 && strcmp(argv[1], "--filename") == 0)
        return print_identity("filename", argv[2]);
    fprintf(stderr,
            "usage: crossnook-bookid-test --binary <file>\n"
            "       crossnook-bookid-test --filename <path>\n"
            "       crossnook-bookid-test --api-smoke\n");
    return 2;
}
