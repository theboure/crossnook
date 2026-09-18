/*
 * library.h — book library from a plain filesystem directory.
 *
 * Non-recursive scan of one directory into a dynamically sized, sorted
 * list of books. No EPUB/FB2 metadata is parsed here yet: the displayed
 * title is the filename without its extension, and the only stored fields
 * are path, filename, title and the detected format.
 *
 * Recognition (case-insensitive extensions): .epub, .fb2, .txt.
 * Ignored: hidden files (leading '.'), directories, unsupported
 * extensions, and temporary files (names containing '#', ending with '~',
 * or ending with ".tmp").
 *
 * Deterministic order: sorted case-insensitively by displayed title
 * (ASCII + Cyrillic folded), with raw-string ties broken by byte order.
 */
#ifndef CN_LIBRARY_LIBRARY_H
#define CN_LIBRARY_LIBRARY_H

typedef enum cn_book_format {
    CN_BOOK_EPUB = 0,
    CN_BOOK_FB2,
    CN_BOOK_TXT
} cn_book_format;

typedef struct cn_book {
    char *path;         /* full path: <dir>/<filename> */
    char *filename;     /* basename, exactly as found */
    char *title;        /* filename without extension (displayed title) */
    cn_book_format format;
} cn_book;

typedef struct cn_library {
    cn_book *books;
    int     count;
    int     cap;
} cn_library;

/* Fixed display names ("EPUB", "FB2", "TXT"). */
const char *cn_book_format_name(cn_book_format f);

/* Empty, unsorted library (count 0). */
cn_library *cn_library_new(void);
void        cn_library_free(cn_library *lib);

/* Append one book (copies strings). Returns its index or -1 on error. */
int cn_library_add(cn_library *lib, const char *path,
                   const char *filename, cn_book_format format);

/* Sort deterministically (see file comment). */
void cn_library_resort(cn_library *lib);

/* Scan `dir` (non-recursive). Returns a valid library with count 0 for an
 * empty directory; returns NULL only if the directory cannot be opened. */
cn_library *cn_library_scan(const char *dir);

int          cn_library_count(const cn_library *lib);
const cn_book *cn_library_get(const cn_library *lib, int i);

#endif /* CN_LIBRARY_LIBRARY_H */