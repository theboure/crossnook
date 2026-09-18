/*
 * input.c — semantic input layer (see input.h).
 *
 * Event mapping and raw-code knowledge is confined to this file:
 *   event0  TWL4030 Keypad: KEY_NEXT 407, KEY_PREVIOUS 412,
 *           KEY_MENU 139, KEY_BACK 158  (semantic on press value==1)
 *   event1  gpio-keys: KEY_HOME 102, KEY_POWER 116
 *           (POWER reports press AND release to the UI for hold timing)
 *   event2  zForce Touch: ABS_X, ABS_Y, BTN_TOUCH, SYN_REPORT
 *
 * zForce frames: the very first down frame and the release frame can carry
 * a transient ABS_Y near the opposite edge (e.g. 300,9 down, 301,781 move,
 * 300,9 up for a real touch near (298,780)). Gesture aggregation therefore
 * emits TOUCH_DOWN with provisional coordinates, tracks the latest
 * in-contact coordinate for TOUCH_MOVE, and resolves the final tap
 * coordinate on TOUCH_UP from the last in-contact frame only (release
 * frame coordinates are never used; a one-frame tap falls back to the
 * down-frame coordinate). Coordinates are clamped to the 600x800 bounds.
 *
 * A small internal queue decouples evdev reads (which can deliver many
 * records per poll()) from the single-event consumer API.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "input.h"
#include "display.h"

#define DEV_E0      "/dev/input/event0"
#define DEV_E1      "/dev/input/event1"
#define DEV_E2      "/dev/input/event2"

#define EV_SYN       0x00
#define EV_KEY       0x01
#define EV_ABS       0x03

#define KEY_MENU     139
#define KEY_HOME     102
#define KEY_POWER    116
#define KEY_PREVIOUS 412
#define KEY_NEXT     407
#define KEY_BACK     158

#define ABS_X        0x00
#define ABS_Y        0x01
#define BTN_TOUCH    330
#define SYN_REPORT   0

#define QUEUE_MAX    64

/* raw evdev record layout (16 bytes on ARM EABI / Linux 2.6.29) */
struct input_event_le {
    int32_t  time_sec;
    int32_t  time_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

struct cn_input {
    int     fds[3];        /* -1 = not open */
    int     nfds;
    cn_input_ev queue[QUEUE_MAX];
    int     q_head, q_tail;
    int     frame_x, frame_y;   /* current frame ABS coordinates */
    int     frame_down;         /* BTN_TOUCH state for this frame */
    int     cur_x, cur_y;       /* latest in-contact coordinate */
    int     gesture;            /* contact currently held */
    int     last_emit_x, last_emit_y; /* coords of last DOWN/MOVE sent */
};

/* Map a raw input event to a semantic one. Returns CN_INPUT_NONE for
 * unmapped events. Mirror of inputmap.h as used by crossnook-test. */
static cn_input_event map_raw(int dev, int type, int code)
{
    if (type == EV_KEY) {
        if (dev == 0) {
            if (code == KEY_NEXT)      return CN_INPUT_PAGE_NEXT;
            if (code == KEY_PREVIOUS)  return CN_INPUT_PAGE_PREV;
            if (code == KEY_MENU)      return CN_INPUT_MENU;
            if (code == KEY_BACK)      return CN_INPUT_BACK;
        } else if (dev == 1) {
            if (code == KEY_HOME)      return CN_INPUT_HOME;
            if (code == KEY_POWER)     return CN_INPUT_POWER_DOWN;
        }
        return CN_INPUT_NONE;
    }
    return CN_INPUT_NONE;
}

static void push(cn_input *in, cn_input_event type, int x, int y, int raw)
{
    int next = (in->q_tail + 1) % QUEUE_MAX;
    if (next == in->q_head) {
        /* queue full: drop the oldest */
        in->q_head = (in->q_head + 1) % QUEUE_MAX;
    }
    in->queue[in->q_tail].type = type;
    in->queue[in->q_tail].x = x;
    in->queue[in->q_tail].y = y;
    in->queue[in->q_tail].raw_dev = raw;
    in->q_tail = next;
}

static int pop(cn_input *in, cn_input_ev *out)
{
    if (in->q_head == in->q_tail)
        return 0;
    *out = in->queue[in->q_head];
    in->q_head = (in->q_head + 1) % QUEUE_MAX;
    return 1;
}

/* Consume one 16-byte evdev record. */
static void consume(cn_input *in, int dev, const struct input_event_le *ev)
{
    cn_input_event sev;

    if (dev == 2) {
        /* zForce touch stream: gather the current frame, emit at SYN. */
        if (ev->type == EV_ABS) {
            if (ev->code == ABS_X) {
                in->frame_x = ev->value;
            } else if (ev->code == ABS_Y) {
                in->frame_y = ev->value;
            }
            return;
        }
        if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
            in->frame_down = ev->value != 0;
            return;
        }
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
            int x, y;
            x = in->frame_x;
            y = in->frame_y;
            if (x < 0) x = 0;
            if (x > CN_FB_W - 1) x = CN_FB_W - 1;
            if (y < 0) y = 0;
            if (y > CN_FB_H - 1) y = CN_FB_H - 1;

            if (in->frame_down) {
                if (!in->gesture) {
                    /* first in-contact frame: provisional start */
                    in->gesture = 1;
                    in->cur_x = x;
                    in->cur_y = y;
                    in->last_emit_x = x;
                    in->last_emit_y = y;
                    push(in, CN_INPUT_TOUCH_DOWN, x, y, dev);
                } else {
                    /* later in-contact frame: track latest coordinate */
                    in->cur_x = x;
                    in->cur_y = y;
                    if (x != in->last_emit_x || y != in->last_emit_y) {
                        push(in, CN_INPUT_TOUCH_MOVE, x, y, dev);
                        in->last_emit_x = x;
                        in->last_emit_y = y;
                    }
                }
            } else if (in->gesture) {
                /* release: resolve from the last in-contact frame only.
                 * A one-frame tap never moved, so cur == down. */
                push(in, CN_INPUT_TOUCH_UP, in->cur_x, in->cur_y, dev);
                in->gesture = 0;
                in->cur_x = in->cur_y = 0;
                in->last_emit_x = in->last_emit_y = 0;
            }
            return;
        }
        return;
    }

    sev = map_raw(dev, ev->type, ev->code);
    if (sev == CN_INPUT_NONE)
        return;

    if (sev == CN_INPUT_POWER_DOWN) {
        push(in, ev->value != 0 ? CN_INPUT_POWER_DOWN : CN_INPUT_POWER_UP,
             0, 0, dev);
        return;
    }

    /* page/menu/back/home: semantic on press (value != 0) only */
    if (ev->value != 0)
        push(in, sev, 0, 0, dev);
}

/* Drain all currently ready fds into the queue. */
static void fill_queue(cn_input *in)
{
    int i;
    for (i = 0; i < 3; i++) {
        struct input_event_le ev;
        ssize_t n;
        if (in->fds[i] < 0)
            continue;
        for (;;) {
            do {
                n = read(in->fds[i], &ev, sizeof ev);
            } while (n < 0 && errno == EINTR);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    fprintf(stderr, "input: read event%d: %s\n",
                            i, strerror(errno));
                break;
            }
            if (n == 0)
                break;
            if (n != (ssize_t)sizeof ev) {
                fprintf(stderr, "input: event%d torn read\n", i);
                break;
            }
            consume(in, i, &ev);
        }
    }
}

static int open_dev(const char *path)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0)
        fprintf(stderr, "input: open %s: %s\n", path, strerror(errno));
    return fd;
}

cn_input *cn_input_open(void)
{
    cn_input *in;
    const char *paths[3] = { DEV_E0, DEV_E1, DEV_E2 };
    int i;

    in = (cn_input *)calloc(1, sizeof *in);
    if (!in)
        return NULL;
    for (i = 0; i < 3; i++)
        in->fds[i] = -1;

    for (i = 0; i < 3; i++)
        in->fds[i] = open_dev(paths[i]);

    in->nfds = 0;
    for (i = 0; i < 3; i++)
        if (in->fds[i] >= 0)
            in->nfds++;
    return in;
}

int cn_input_poll(cn_input *in, cn_input_ev *out, int timeout_ms)
{
    struct pollfd pfds[3];
    int i, np = 0;
    int rc;

    if (!in)
        return -1;

    if (pop(in, out))
        return 1;                   /* deliver queued events first */

    if (in->nfds == 0)
        return -1;

    for (i = 0; i < 3; i++) {
        if (in->fds[i] < 0)
            continue;
        pfds[np].fd = in->fds[i];
        pfds[np].events = POLLIN;
        pfds[np].revents = 0;
        np++;
    }

    rc = poll(pfds, np, timeout_ms);
    if (rc < 0) {
        if (errno == EINTR)
            return 0;
        fprintf(stderr, "input: poll: %s\n", strerror(errno));
        return -1;
    }
    if (rc == 0)
        return 0;

    fill_queue(in);
    return pop(in, out) ? 1 : 0;
}

int cn_input_devices(const cn_input *in)
{
    return in ? in->nfds : 0;
}

void cn_input_close(cn_input *in)
{
    int i;
    if (!in)
        return;
    for (i = 0; i < 3; i++)
        if (in->fds[i] >= 0)
            close(in->fds[i]);
    free(in);
}