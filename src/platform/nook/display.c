/*
 * display.c — Nook Simple Touch framebuffer display abstraction.
 *
 * The device node path lives only here; nothing else in the tree may
 * reference /dev/graphics/fb0 (see the structural check in build-ui.sh).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "display.h"

static const char FB_PATH[] = "/dev/graphics/fb0";

struct cn_display {
    int fd;
};

/* Partial-write-safe full write (validated in the hardware-bringup
 * milestone): EINTR retries, 0-byte writes abort. */
static int write_all(int fd, const void *buf, size_t len)
{
    const unsigned char *p = buf;

    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

cn_display *cn_display_open(void)
{
    cn_display *d;
    int fd;

    fd = open(FB_PATH, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "display: open %s: %s\n", FB_PATH, strerror(errno));
        return NULL;
    }
    d = (cn_display *)malloc(sizeof *d);
    if (!d) {
        close(fd);
        return NULL;
    }
    d->fd = fd;
    return d;
}

int cn_display_flush(cn_display *d, const void *pixels)
{
    if (!d)
        return -1;
    if (lseek(d->fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "display: lseek: %s\n", strerror(errno));
        return -1;
    }
    if (write_all(d->fd, pixels, CN_FB_BYTES) != 0) {
        fprintf(stderr, "display: fb write: %s\n", strerror(errno));
        return -1;
    }
    return CN_FB_BYTES;
}

void cn_display_close(cn_display *d)
{
    if (!d)
        return;
    close(d->fd);
    free(d);
}