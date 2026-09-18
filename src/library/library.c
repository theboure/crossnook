/*
 * library.c — book library scan (see library.h).
 */
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "library.h"

#define CAP_INIT 8

/* ---- string helpers --------------------------------------------- */

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c;
}

static int strieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (ascii_lower((unsigned char)*a) !=
            ascii_lower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int strieq_end(const char *s, const char *suffix)
{
    size_t ls = strlen(s), lx = strlen(suffix);
    if (ls < lx)
        return 0;
    return strieq(s + ls - lx, suffix);
}

/* Detect the format from the trailing extension (case-insensitive).
 * Returns 1 and fills *out on a supported extension, else 0. */
static int format_of(const char *name, cn_book_format *out)
{
    const char *dot = strrchr(name, '.');

    if (!dot || dot == name || dot[1] == '\0')
        return 0;                  /* no / empty extension: unsupported */
    if (strieq(dot + 1, "epub")) { *out = CN_BOOK_EPUB; return 1; }
    if (strieq(dot + 1, "fb2"))  { *out = CN_BOOK_FB2;  return 1; }
    if (strieq(dot + 1, "txt"))  { *out = CN_BOOK_TXT;  return 1; }
    return 0;
}

static int is_temp_file(const char *name)
{
    if (strchr(name, '#') != NULL)
        return 1;
    if (name[strlen(name) - 1] == '~')
        return 1;
    if (strieq_end(name, ".tmp"))
        return 1;
    return 0;
}

/* Title = filename without its trailing extension. */
static char *title_of(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    size_t n = dot ? (size_t)(dot - filename) : strlen(filename);
    char *t = malloc(n + 1);

    if (t) {
        memcpy(t, filename, n);
        t[n] = '\0';
    }
    return t;
}

/* ---- deterministic case-insensitive sort ------------------------ */

/* Fold one grapheme: ASCII A-Z -> a-z, Cyrillic upper -> lower codepoint.
 * Returns -1 at end of string, else the folded value (>0xFF for
 * Cyrillic, which therefore always sorts after ASCII). */
static int fold_next(const char **sp)
{
    const unsigned char *p = (const unsigned char *)*sp;
    unsigned char c = p[0];

    if (c == 0)
        return -1;

    if (c < 0x80) {                 /* ASCII */
        (*sp)++;
        return ascii_lower(c);
    }
    if (c == 0xD0 && p[1]) {        /* Ё + А-Я */
        unsigned char b = p[1];
        (*sp) += 2;
        if (b == 0x81) return 0x451;        /* Ё -> ё */
        if (b >= 0x90 && b <= 0xAF) return 0x430 + (b - 0x90);  /* -> а-я */
        if (b >= 0xB0 && b <= 0xBF) return 0x430 + (b - 0xB0);  /* а-п */
        return 0xD000 + b;                  /* other D0 second byte */
    }
    if (c == 0xD1 && p[1]) {        /* р-я, ё */
        unsigned char b = p[1];
        (*sp) += 2;
        if (b >= 0x80 && b <= 0x8F) return 0x440 + (b - 0x80);  /* р-я */
        if (b == 0x91) return 0x451;        /* ё */
        return 0xD100 + b;
    }
    (*sp)++;
    return c;
}

static int fold_cmp(const char *a, const char *b)
{
    for (;;) {
        int fa = fold_next(&a);
        int fb = fold_next(&b);
        if (fa != fb)
            return fa < fb ? -1 : 1;
        if (fa < 0)
            return 0;
    }
}

static int book_cmp(const void *pa, const void *pb)
{
    const cn_book *a = (const cn_book *)pa;
    const cn_book *b = (const cn_book *)pb;
    int c = fold_cmp(a->title, b->title);
    if (c)
        return c;
    c = strcmp(a->title, b->title);         /* stable tie-break */
    if (c)
        return c;
    return strcmp(a->filename, b->filename);
}

/* ---- public API --------------------------------------------------- */

const char *cn_book_format_name(cn_book_format f)
{
    switch (f) {
    case CN_BOOK_EPUB: return "EPUB";
    case CN_BOOK_FB2:  return "FB2";
    case CN_BOOK_TXT:  return "TXT";
    default:           return "?";
    }
}

cn_library *cn_library_new(void)
{
    return (cn_library *)calloc(1, sizeof(cn_library));
}

void cn_library_free(cn_library *lib)
{
    int i;
    if (!lib)
        return;
    for (i = 0; i < lib->count; i++) {
        free(lib->books[i].path);
        free(lib->books[i].filename);
        free(lib->books[i].title);
    }
    free(lib->books);
    free(lib);
}

int cn_library_add(cn_library *lib, const char *path,
                   const char *filename, cn_book_format format)
{
    cn_book b;
    cn_book *nb;

    if (!lib)
        return -1;
    if (lib->count == lib->cap) {
        int ncap = lib->cap ? lib->cap * 2 : CAP_INIT;
        nb = (cn_book *)realloc(lib->books, (size_t)ncap * sizeof(cn_book));
        if (!nb)
            return -1;
        lib->books = nb;
        lib->cap = ncap;
    }
    b.path = strdup(path);
    b.filename = strdup(filename);
    b.title = title_of(filename);
    b.format = format;
    if (!b.path || !b.filename || !b.title) {
        free(b.path);
        free(b.filename);
        free(b.title);
        return -1;
    }
    lib->books[lib->count] = b;
    return lib->count++;
}

void cn_library_resort(cn_library *lib)
{
    if (lib && lib->count > 1)
        qsort(lib->books, (size_t)lib->count, sizeof(cn_book), book_cmp);
}

static int is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

cn_library *cn_library_scan(const char *dir)
{
    cn_library *lib = NULL;
    DIR *d;
    struct dirent *de;

    d = opendir(dir);
    if (!d) {
        fprintf(stderr, "library: open %s: %s\n", dir, strerror(errno));
        return NULL;
    }
    lib = cn_library_new();
    if (!lib) {
        closedir(d);
        return NULL;
    }

    while ((de = readdir(d)) != NULL) {
        const char *name = de->d_name;
        cn_book_format fmt;
        char *path;
        size_t dlen, nlen;

        if (name[0] == '.')                 /* hidden */
            continue;
        if (is_temp_file(name))
            continue;
        if (!format_of(name, &fmt))
            continue;

        dlen = strlen(dir);
        nlen = strlen(name);
        path = malloc(dlen + 1 + nlen + 1);
        if (!path)
            goto oom;
        memcpy(path, dir, dlen);
        path[dlen] = '/';
        memcpy(path + dlen + 1, name, nlen + 1);

        if (de->d_type == DT_DIR) {         /* directory */
            free(path);
            continue;
        }
        if (de->d_type == DT_UNKNOWN) {
            /* filesystems without d_type (or DT_LNK etc.): stat it */
            int st = is_directory(path);
            if (st < 0 || st == 1) {        /* stat error or directory */
                free(path);
                continue;
            }
        } else if (de->d_type != DT_REG) {
            free(path);                     /* sockets, links, ... */
            continue;
        }

        if (cn_library_add(lib, path, name, fmt) < 0) {
            free(path);
            goto oom;
        }
        free(path);
    }
    closedir(d);
    cn_library_resort(lib);
    return lib;

oom:
    fprintf(stderr, "library: out of memory scanning %s\n", dir);
    closedir(d);
    cn_library_free(lib);
    return NULL;
}

int cn_library_count(const cn_library *lib)
{
    return lib ? lib->count : 0;
}

const cn_book *cn_library_get(const cn_library *lib, int i)
{
    if (!lib || i < 0 || i >= lib->count)
        return NULL;
    return &lib->books[i];
}