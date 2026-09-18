/*
 * crossnook-test.c — diagnostic fb + input test app (single native ARM
 * binary, statically linked, no mmap / no fb ioctls / no sysfs EPD).
 *
 * Target: Nook Simple Touch (BNRV300), kernel 2.6.29, 600x800 RGB565
 * framebuffer at /dev/graphics/fb0, evdev at event0/1/2.
 *
 * Behavioral spec (see docs/crossnook-test.md):
 *   - software framebuffer uint16_t fb[800][600];
 *   - refresh = lseek(fb,0,SEEK_SET) + write all 960000 bytes;
 *   - poll() over event0/event1/event2;
 *   - business logic sees only semantic events from inputmap.h;
 *   - MAIN screen "CROSSNOOK TEST / PAGE n";
 *   - PAGE_NEXT/+/PAGE_PREV- floor 1; MENU/BACK toggle diagnostic states;
 *   - HOME toggles MAIN/HOME screens; touch draws centered marker;
 *   - short POWER does nothing; long POWER (>= 2000 ms press..release) exits.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "font5x7.h"
#include "inputmap.h"

/* ---- constants ------------------------------------------------ */

#define FB_W        600
#define FB_H        800
#define FB_BYTES    (FB_W * FB_H * 2)   /* 960000 */
#define FB_PATH     "/dev/graphics/fb0"

#define DEV_E0      "/dev/input/event0"
#define DEV_E1      "/dev/input/event1"
#define DEV_E2      "/dev/input/event2"

#define COLOR_WHITE 0xFFFF
#define COLOR_BLACK 0x0000
#define COLOR_GRAY  0x7BEF

#define POWER_LONG_MS 2000     /* documented long-press threshold */

#define SCREEN_MAIN 0
#define SCREEN_HOME 1

/* raw evdev record layout (16 bytes on ARM EABI / Linux 2.6.29) */
struct input_event_le {
    int32_t  time_sec;
    int32_t  time_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

/* ---- state ----------------------------------------------------- */

static uint16_t fb[FB_H][FB_W];

static int screen = SCREEN_MAIN;
static int page = 1;

static int diag_menu = 0;      /* MENU toggle: black band          */
static int diag_back = 0;      /* BACK toggle: checker pad         */

static int mark_x = -1;        /* last touch point, -1 = none */
static int mark_y = -1;

static int cur_touch_x = 0;    /* current frame ABS_X */
static int cur_touch_y = 0;    /* current frame ABS_Y */
static int cur_touch_down = 0; /* current frame BTN_TOUCH */

static int64_t power_press_ms = -1;   /* monotonic ms at KEY_POWER down */
static int64_t power_last_ms  = -1;   /* ms of last power release/cycle */
static int     power_is_long  = 0;
static int     exit_requested = 0;    /* set when POWER long-press ends */

/* ---- time helper ----------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- drawing primitives (rectangles only + font) ---------------- */

static void clear(void)
{
    memset(fb, (int)0xFF, sizeof fb);       /* white */
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    int x, y;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= FB_W) x1 = FB_W - 1;
    if (y1 >= FB_H) y1 = FB_H - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++)
            fb[y][x] = color;
}

static void put_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= FB_W || y < 0 || y >= FB_H)
        return;
    fb[y][x] = color;
}

/* 5x7 font; row = 1 byte, bit 4 (0x10) is the leftmost pixel. */
static void draw_char(int x, int y, unsigned char c, int scale,
                      uint16_t fg, uint16_t bg)
{
    const unsigned char *g;
    int row, col, dx, dy;
    if (c < FONT_START || c >= FONT_START + FONT_NGLYPHS)
        c = '?';
    g = font5x7[c - FONT_START];
    for (row = 0; row < 7; row++) {
        for (col = 0; col < 5; col++) {
            uint16_t color = (g[row] & (0x10 >> col)) ? fg : bg;
            for (dy = 0; dy < scale; dy++)
                for (dx = 0; dx < scale; dx++)
                    put_pixel(x + col * scale + dx,
                              y + row * scale + dy, color);
        }
    }
}

static void draw_text(int x, int y, const char *s, int scale,
                      uint16_t fg, uint16_t bg)
{
    while (*s) {
        draw_char(x, y, (unsigned char)*s, scale, fg, bg);
        x += 6 * scale;
        s++;
    }
}

/* ---- framebuffer refresh --------------------------------------- */

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

/* lseek to 0 then push all 960000 bytes; correct for partial writes. */
static int fb_refresh(int fb_fd)
{
    if (lseek(fb_fd, 0, SEEK_SET) < 0)
        return -1;
    if (write_all(fb_fd, fb, sizeof fb) != 0)
        return -1;
    return FB_BYTES;
}

/* ---- screen rendering ------------------------------------------- */

static void draw_footer(void)
{
    draw_text(8, FB_H - 22,
              "PAGE- = L-UP  PAGE+ = R-UP  MENU = L-LOW", 1,
              COLOR_BLACK, COLOR_WHITE);
    draw_text(8, FB_H - 12,
              "BACK = R-LOW  HOME = HOME  PWR = HOLD2S>EXIT", 1,
              COLOR_BLACK, COLOR_WHITE);
}

static void draw_touch_marker(void)
{
    int r = 6;  /* half-size of marker box */
    if (mark_x < 0 || mark_y < 0)
        return;
    /* outer black square centered on the touch point */
    fill_rect(mark_x - r, mark_y - r, mark_x + r, mark_y + r, COLOR_BLACK);
    /* inner white dot so the marker stays visible on black */
    fill_rect(mark_x - 1, mark_y - 1, mark_x + 1, mark_y + 1, COLOR_WHITE);
}

static void draw_diag_bands(void)
{
    int x, y;

    /* MENU diagnostic state: full-width black band, center-right slab */
    if (diag_menu) {
        fill_rect(FB_W - 90, 20, FB_W - 8, 90, COLOR_BLACK);
        draw_text(FB_W - 86, 42, "MENU", 2, COLOR_WHITE, COLOR_BLACK);
        fill_rect(0, FB_H - 60, FB_W - 1, FB_H - 60 + 4, COLOR_BLACK);
    }

    /* BACK diagnostic state: checkerboard pad, bottom-left */
    if (diag_back) {
        for (y = FB_H - 110; y < FB_H - 90; y++)
            for (x = 10; x < 90; x++)
                fb[y][x] = ((x - 10) / 10 + (y - (FB_H - 110)) / 10) % 2
                               ? COLOR_BLACK : COLOR_WHITE;
        draw_text(10, FB_H - 76, "BACK", 2, COLOR_BLACK, COLOR_WHITE);
    }
}

static void draw_screen(void)
{
    char buf[40];

    clear();
    draw_footer();
    draw_diag_bands();

    if (screen == SCREEN_MAIN) {
        draw_text(10, 14, "CROSSNOOK TEST", 3, COLOR_BLACK, COLOR_WHITE);
        draw_text(10, 14 + 22, "DIAGNOSTIC BUILD", 1, COLOR_BLACK, COLOR_WHITE);
        snprintf(buf, sizeof buf, "PAGE %d", page);
        draw_text(10, 130, buf, 2, COLOR_BLACK, COLOR_WHITE);
        draw_text(10, 170, "MAIN SCREEN", 1, COLOR_BLACK, COLOR_WHITE);
    } else {
        draw_text(10, 14, "HOME SCREEN", 3, COLOR_BLACK, COLOR_WHITE);
        draw_text(10, 14 + 22, "PRESS HOME TO RETURN", 1,
                  COLOR_BLACK, COLOR_WHITE);
        snprintf(buf, sizeof buf, "PAGE %d", page);
        draw_text(10, 130, buf, 2, COLOR_BLACK, COLOR_WHITE);
        draw_text(10, 170, "SCREEN=HOME", 1, COLOR_BLACK, COLOR_WHITE);
    }

    if (power_last_ms >= 0) {
        snprintf(buf, sizeof buf, "PWR %lldms %s",
                 (long long)power_last_ms,
                 power_is_long ? "LONG->EXIT" : "SHORT");
        draw_text(10, 210, buf, 1, COLOR_BLACK, COLOR_WHITE);
    }

    draw_touch_marker();
}

/* ---- semantic event handling ------------------------------------ */

/* returns 1 when the screen must be redrawn, 0 when not */
static int handle_sev(enum sev_action act, int value)
{
    switch (act) {
    case SEV_PAGE_NEXT:
        if (value == 1) {
            page++;
            return 1;
        }
        return 0;

    case SEV_PAGE_PREV:
        if (value == 1) {
            if (page > 1)
                page--;
            return 1;
        }
        return 0;

    case SEV_MENU:
        if (value == 1) {
            diag_menu = !diag_menu;
            return 1;
        }
        return 0;

    case SEV_BACK:
        if (value == 1) {
            diag_back = !diag_back;
            return 1;
        }
        return 0;

    case SEV_HOME:
        if (value == 1) {
            screen = (screen == SCREEN_MAIN) ? SCREEN_HOME : SCREEN_MAIN;
            return 1;
        }
        return 0;

    case SEV_POWER:
        if (value == 1) {
            power_press_ms = now_ms();
            return 0;               /* nothing drawn on press itself */
        }
        if (value == 0 && power_press_ms >= 0) {
            power_last_ms = now_ms() - power_press_ms;
            power_is_long = (power_last_ms >= POWER_LONG_MS);
            if (power_is_long)
                exit_requested = 1;
            power_press_ms = -1;
            return 1;               /* show duration + SHORT/LONG */
        }
        return 0;

    case SEV_TOUCH_X:
        cur_touch_x = value;
        return 0;

    case SEV_TOUCH_Y:
        cur_touch_y = value;
        return 0;

    case SEV_TOUCH_DOWN:
        cur_touch_down = value;
        return 0;

    case SEV_FRAME:
        /* zForce emits ABS_X/ABS_Y/BTN_TOUCH then SYN_REPORT. Only a
         * completed frame with touch down commits the marker. */
        if (value == 0 && cur_touch_down) {
            if (cur_touch_x != mark_x || cur_touch_y != mark_y) {
                mark_x = cur_touch_x;
                mark_y = cur_touch_y;
                return 1;
            }
        }
        return 0;

    default:
        return 0;
    }
}

/* ---- evdev reading ----------------------------------------------- */

/* returns 1 event read, 0 = EAGAIN (drained), -1 = error/eof */
static int read_event(int fd, struct input_event_le *ev)
{
    ssize_t n;
    do {
        n = read(fd, ev, sizeof *ev);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (n == 0)
        return -1;                  /* device removed */
    if (n != (ssize_t)sizeof *ev)
        return -1;                  /* torn read: abort this device */
    return 1;
}

static int open_dev(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        fprintf(stderr, "crossnook-test: open %s: %s\n", path, strerror(errno));
    return fd;
}

/* ---- self-check (host-side validation, no device needed) -------- */

static int self_check(const char *out_path)
{
    uint32_t sum = 0;
    int fd, i, rc;

    page = 7;
    diag_menu = 1;
    diag_back = 1;
    mark_x = 300;
    mark_y = 400;
    power_is_long = 1;
    power_last_ms = 2530;
    draw_screen();

    for (i = 0; i < FB_W * FB_H; i++)
        sum += ((const uint16_t *)fb)[i];

    fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        perror("open output");
        return 1;
    }
    rc = fb_refresh(fd);
    close(fd);
    if (rc != FB_BYTES) {
        fprintf(stderr, "SELFCHECK fb_refresh rc=%d\n", rc);
        return 1;
    }

    printf("SELFCHECK page=%d menudiag=%d backdiag=%d mark=%d,%d "
           "sum=%08x bytes=%d\n",
           page, diag_menu, diag_back, mark_x, mark_y, sum, FB_BYTES);
    return 0;
}

/* ---- main -------------------------------------------------------- */

int main(int argc, char **argv)
{
    struct pollfd pfds[3];
    int roles[3] = { -1, -1, -1 };
    int nfds = 0;
    int fb_fd;
    int e0, e1, e2;
    int running = 1;

    if (argc == 3 && strcmp(argv[1], "--selfcheck") == 0)
        return self_check(argv[2]);

    fb_fd = open(FB_PATH, O_WRONLY);
    if (fb_fd < 0) {
        fprintf(stderr, "crossnook-test: open %s: %s\n",
                FB_PATH, strerror(errno));
        return 1;
    }

    e0 = open_dev(DEV_E0);
    e1 = open_dev(DEV_E1);
    e2 = open_dev(DEV_E2);

    /* poll fds; roles[i] remembers which eventX (0/1/2) each fd is,
     * because mapping depends on the device, not the poll index. */
    if (e0 >= 0) {
        pfds[nfds].fd = e0;
        pfds[nfds].events = POLLIN;
        roles[nfds] = 0;
        nfds++;
    }
    if (e1 >= 0) {
        pfds[nfds].fd = e1;
        pfds[nfds].events = POLLIN;
        roles[nfds] = 1;
        nfds++;
    }
    if (e2 >= 0) {
        pfds[nfds].fd = e2;
        pfds[nfds].events = POLLIN;
        roles[nfds] = 2;
        nfds++;
    }

    fprintf(stderr, "crossnook-test: fb=%s ok, %d input device(s) open\n",
            FB_PATH, nfds);

    /* initial frame */
    draw_screen();
    if (fb_refresh(fb_fd) != FB_BYTES) {
        fprintf(stderr, "crossnook-test: initial fb write failed: %s\n",
                strerror(errno));
        close(fb_fd);
        return 1;
    }

    while (running) {
        int rpoll, i;
        int changed = 0;

        if (exit_requested)
            break;

        rpoll = poll(pfds, nfds, -1);
        if (rpoll < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll: %s\n", strerror(errno));
            break;
        }

        for (i = 0; i < nfds; i++) {
            struct input_event_le ev;
            int rc;
            int devsel = roles[i];

            if (!(pfds[i].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            if (pfds[i].revents & (POLLERR | POLLHUP)) {
                fprintf(stderr, "crossnook-test: input device %d gone\n", i);
                pfds[i].fd = -1;
                continue;
            }

            while ((rc = read_event(pfds[i].fd, &ev)) > 0) {
                enum sev_action act;
                int w = ev.value;
                act = inputmap_map(devsel, ev.type, ev.code, ev.value, &w);
                if (act != SEV_NONE && handle_sev(act, w))
                    changed = 1;
            }
            if (rc < 0) {
                fprintf(stderr, "crossnook-test: read event%d: %s\n",
                        devsel, strerror(errno));
            }
        }

        if (changed) {
            draw_screen();
            if (fb_refresh(fb_fd) != FB_BYTES) {
                fprintf(stderr, "crossnook-test: fb write failed: %s\n",
                        strerror(errno));
                break;
            }
        }

        if (exit_requested) {
            fprintf(stderr, "crossnook-test: POWER long-press, exiting\n");
            running = 0;
        }
    }

    close(fb_fd);
    if (e0 >= 0) close(e0);
    if (e1 >= 0) close(e1);
    if (e2 >= 0) close(e2);
    return 0;
}