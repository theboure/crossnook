/*
 * reader-position-test.c - logical ReaderPosition host/device validation.
 *
 * This exercises only the public reader C API. It never opens framebuffer or
 * input devices, so the same static ARM binary runs under qemu-arm and on the
 * Nook Simple Touch.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "reader/reader.h"

#define GUARD_BYTES 64
#define PAGE_BYTES ((size_t)CN_READER_W * CN_READER_H * 2)
#define OVERLONG_POSITION_BYTES 65538

static uint32_t checksum(const uint8_t *data, size_t length)
{
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0; i < length; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static int render_guarded(cn_reader *reader, uint32_t *result)
{
    uint8_t *base = (uint8_t *)malloc(GUARD_BYTES + PAGE_BYTES + GUARD_BYTES);
    uint8_t *target;
    size_t i;
    int failed = 0;

    if (!base)
        return -1;
    memset(base, 0x5a, GUARD_BYTES + PAGE_BYTES + GUARD_BYTES);
    target = base + GUARD_BYTES;
    if (cn_reader_render(reader, target, CN_READER_W, CN_READER_H) != 0)
        failed = 1;
    for (i = 0; i < GUARD_BYTES; ++i) {
        if (base[i] != 0x5a || base[GUARD_BYTES + PAGE_BYTES + i] != 0x5a) {
            failed = 1;
            break;
        }
    }
    if (!failed)
        *result = checksum(target, PAGE_BYTES);
    free(base);
    return failed ? -1 : 0;
}

static void check(int condition, const char *name, int *failures)
{
    printf("[%s] %s\n", condition ? "OK" : "FAIL", name);
    if (!condition)
        (*failures)++;
}

/* Add a leading zero to one canonical [index]. CREngine resolves the same
 * node, but canonical reserialization must differ and the Reader must reject
 * the token without moving. */
static char *make_noncanonical(const char *canonical)
{
    const char *index;
    char *result;
    size_t prefix;
    size_t length;
    if (!canonical)
        return NULL;
    index = strchr(canonical, '[');
    if (!index || index[1] < '0' || index[1] > '9')
        return NULL;
    length = strlen(canonical);
    prefix = (size_t)(index - canonical) + 1;
    result = (char *)malloc(length + 2);
    if (!result)
        return NULL;
    memcpy(result, canonical, prefix);
    result[prefix] = '0';
    memcpy(result + prefix + 1, canonical + prefix, length - prefix + 1);
    return result;
}

static int run_smoke(const char *font, const char *book, const char *foreign)
{
    cn_reader_config config_a;
    cn_reader_config config_b;
    cn_reader_position saved;
    cn_reader_position copied;
    cn_reader_position recaptured;
    cn_reader_position empty = { 0 };
    cn_reader *reader;
    char malformed_location[] = "/body/DocFragment[not-a-number]/text().0";
    cn_reader_position malformed = { malformed_location, 0 };
    char unresolved_location[] = "/body/DocFragment[999999]/text().0";
    cn_reader_position unresolved = { unresolved_location, 0 };
    char overflow_location[] = "/body/DocFragment[2147483648]/text().0";
    cn_reader_position overflow = { overflow_location, 0 };
    char invalid_utf8_location[] = { '/', (char)0xc0, (char)0xaf, '\0' };
    cn_reader_position invalid_utf8 = { invalid_utf8_location, 0 };
    cn_reader_position noncanonical = { NULL, 0 };
    char *overlong_location;
    cn_reader_position overlong = { NULL, 0 };
    uint32_t before_checksum = 0;
    uint32_t reopened_checksum = 0;
    uint32_t relayout_checksum = 0;
    uint32_t failure_checksum = 0;
    int page_a;
    int page_b;
    int pages_a;
    int pages_b;
    int before_failure_page;
    int failures = 0;

    memset(&config_a, 0, sizeof config_a);
    config_a.font_path = font;
    memset(&config_b, 0, sizeof config_b);
    config_b.font_path = font;
    config_b.font_size = 40;
    config_b.margin_px = 24;

    cn_reader_position_init(&saved);
    cn_reader_position_init(&copied);
    cn_reader_position_init(&recaptured);

    reader = cn_reader_new(&config_a);
    check(reader != NULL, "reader created", &failures);
    if (!reader)
        return 1;
    overlong_location = (char *)malloc(OVERLONG_POSITION_BYTES);
    check(overlong_location != NULL, "overlong-position probe allocated",
          &failures);
    if (overlong_location) {
        memset(overlong_location, 'a', OVERLONG_POSITION_BYTES);
        overlong_location[0] = '/';
        overlong_location[OVERLONG_POSITION_BYTES - 1] = '\0';
        overlong.location = overlong_location;
    }

    check(cn_reader_get_position(reader, &saved) == -1,
          "capture while closed rejected", &failures);
    check(cn_reader_goto_position(reader, &empty) == -1,
          "empty position while closed rejected", &failures);
    check(cn_reader_open(reader, book) == 0, "primary EPUB opened", &failures);
    if (!cn_reader_is_open(reader)) {
        free(overlong_location);
        cn_reader_free(reader);
        return 1;
    }

    pages_a = cn_reader_pages(reader);
    page_a = pages_a / 3;
    if (page_a < 3)
        page_a = 3;
    cn_reader_go(reader, page_a);
    page_a = cn_reader_page(reader);
    check(page_a > 1 && page_a < pages_a - 1,
          "nontrivial physical page selected", &failures);
    check(render_guarded(reader, &before_checksum) == 0,
          "initial render stays within RGB565 buffer", &failures);
    check(cn_reader_get_position(reader, &saved) == 0,
          "logical position captured", &failures);
    check(saved.location && saved.location[0] == '/',
          "logical position is a nonempty opaque token", &failures);
    check(saved.progress_10000 >= 0 && saved.progress_10000 <= 10000,
          "normalized progress metadata is in range", &failures);
    check(cn_reader_position_copy(&copied, &saved) == 0 &&
          copied.location && copied.location != saved.location &&
          strcmp(copied.location, saved.location) == 0 &&
          copied.progress_10000 == saved.progress_10000,
          "position copy owns an independent string", &failures);
    noncanonical.location = make_noncanonical(saved.location);
    check(noncanonical.location != NULL,
          "noncanonical-but-resolvable probe created", &failures);

    cn_reader_close(reader);
    check(cn_reader_open(reader, book) == 0,
          "same EPUB reopened with config A", &failures);
    check(cn_reader_goto_position(reader, &saved) == 0,
          "logical position restored after close/reopen", &failures);
    check(cn_reader_page(reader) == page_a,
          "same layout maps position to same physical page", &failures);
    check(render_guarded(reader, &reopened_checksum) == 0,
          "reopened render stays within RGB565 buffer", &failures);
    check(reopened_checksum == before_checksum,
          "same-layout restore renders the same page", &failures);
    check(cn_reader_get_position(reader, &recaptured) == 0 &&
          recaptured.location && strcmp(recaptured.location, saved.location) == 0,
          "same-layout recapture returns the same logical token", &failures);

    cn_reader_close(reader);
    check(cn_reader_apply_config(reader, &config_b) == 0,
          "layout-changing config accepted while closed", &failures);
    check(cn_reader_open(reader, book) == 0,
          "same EPUB reopened with config B", &failures);
    pages_b = cn_reader_pages(reader);
    check(pages_b != pages_a, "font/margin change alters page count", &failures);
    check(cn_reader_goto_position(reader, &saved) == 0,
          "canonical token resolves exactly after relayout", &failures);
    page_b = cn_reader_page(reader);
    check(saved.location && copied.location &&
          strcmp(saved.location, copied.location) == 0,
          "canonical logical token is unchanged by relayout", &failures);
    check(page_b != page_a,
          "logical location maps to a different physical page", &failures);
    check(render_guarded(reader, &relayout_checksum) == 0,
          "relayout restore stays within RGB565 buffer", &failures);
    check(relayout_checksum != 0 && relayout_checksum != before_checksum,
          "relayout renders the new pagination", &failures);
    printf("POSITION relayout pages=%d->%d page=%d->%d progress=%d\n",
           pages_a, pages_b, page_a, page_b, saved.progress_10000);

    before_failure_page = cn_reader_page(reader);
    check(cn_reader_goto_position(reader, &empty) == -1 &&
          cn_reader_page(reader) == before_failure_page,
          "empty/default position fails without moving", &failures);
    check(cn_reader_goto_position(reader, &malformed) == -1 &&
          cn_reader_page(reader) == before_failure_page,
          "malformed syntax fails before CREngine parsing", &failures);
    check(cn_reader_goto_position(reader, &unresolved) == -1 &&
          cn_reader_page(reader) == before_failure_page,
          "unresolved position fails without moving", &failures);
    if (noncanonical.location) {
        check(cn_reader_goto_position(reader, &noncanonical) == -1 &&
              cn_reader_page(reader) == before_failure_page,
              "noncanonical resolved position fails without moving",
              &failures);
    }
    check(cn_reader_goto_position(reader, &overflow) == -1 &&
          cn_reader_page(reader) == before_failure_page,
          "overflowing position fails before CREngine parsing", &failures);
    check(cn_reader_goto_position(reader, &invalid_utf8) == -1 &&
          cn_reader_page(reader) == before_failure_page,
          "invalid UTF-8 position fails without moving", &failures);
    if (overlong.location) {
        check(cn_reader_goto_position(reader, &overlong) == -1 &&
              cn_reader_page(reader) == before_failure_page,
              "overlong position fails without moving", &failures);
    }
    check(render_guarded(reader, &failure_checksum) == 0 &&
          failure_checksum == relayout_checksum,
          "failed restores leave rendered state unchanged", &failures);

    cn_reader_close(reader);
    check(cn_reader_open(reader, foreign) == 0,
          "foreign EPUB opened", &failures);
    before_failure_page = cn_reader_page(reader);
    check(cn_reader_goto_position(reader, &saved) == -1 &&
          cn_reader_page(reader) == before_failure_page,
          "position from structurally different document fails safely",
          &failures);
    check(render_guarded(reader, &failure_checksum) == 0,
          "foreign document remains renderable and guarded", &failures);

    cn_reader_close(reader);
    check(!cn_reader_is_open(reader) && cn_reader_pages(reader) == 0,
          "reader lifecycle closes cleanly", &failures);
    check(cn_reader_get_position(reader, &recaptured) == -1,
          "capture after close rejected", &failures);

    cn_reader_position_clear(&recaptured);
    cn_reader_position_clear(&copied);
    cn_reader_position_clear(&saved);
    cn_reader_position_clear(&saved);
    free(noncanonical.location);
    free(overlong_location);
    cn_reader_free(reader);

    printf("POSITION SMOKE failures=%d -> %s\n",
           failures, failures == 0 ? "OK" : "FAIL");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "--smoke") == 0)
        return run_smoke(argv[2], argv[3], argv[4]);
    fprintf(stderr,
            "usage: crossnook-position-test --smoke <font> <book> <foreign-book>\n");
    return 2;
}
