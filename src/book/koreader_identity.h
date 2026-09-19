/* KOReader-compatible document identity for future synchronization. */
#ifndef CN_BOOK_KOREADER_IDENTITY_H
#define CN_BOOK_KOREADER_IDENTITY_H

#define CN_KOREADER_DOCUMENT_ID_BYTES 33
#define CN_KOREADER_IDENTITY_PATH_MAX_BYTES 4096

typedef struct cn_koreader_document_id {
    char hex[CN_KOREADER_DOCUMENT_ID_BYTES];
} cn_koreader_document_id;

typedef enum cn_koreader_identity_result {
    CN_KOREADER_IDENTITY_OK = 0,
    CN_KOREADER_IDENTITY_INVALID,
    CN_KOREADER_IDENTITY_IO_ERROR
} cn_koreader_identity_result;

void cn_koreader_document_id_init(cn_koreader_document_id *identity);

/* KOReader KOSync Binary mode: MD5 of exact sampled file bytes. */
cn_koreader_identity_result cn_book_identity_koreader_binary(
    const char *path, cn_koreader_document_id *identity);

/* KOReader KOSync Filename mode: MD5 of bytes after the final '/'. */
cn_koreader_identity_result cn_book_identity_koreader_filename(
    const char *path, cn_koreader_document_id *identity);

const char *cn_koreader_document_id_text(
    const cn_koreader_document_id *identity);
const char *cn_koreader_identity_result_name(
    cn_koreader_identity_result result);

#endif /* CN_BOOK_KOREADER_IDENTITY_H */
