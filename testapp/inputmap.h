/*
 * inputmap.h — semantic input mapping layer for crossnook-test.
 *
 * Business logic (crossnook-test.c) consumes only these semantic events;
 * raw Linux key/abs codes live exclusively in this header.
 *
 * Devices (measured on the real Nook Simple Touch):
 *   event0  TWL4030 Keypad   page buttons
 *   event1  gpio-keys        Home / Power
 *   event2  zForce Touch     ABS_X / ABS_Y / BTN_TOUCH / SYN_REPORT
 */
#ifndef INPUTMAP_H
#define INPUTMAP_H

#include <stdint.h>

/* Semantic events delivered to the app. */
enum sev_action {
    SEV_NONE = 0,

    SEV_PAGE_NEXT, /* right upper page button (KEY_NEXT, 407) */
    SEV_PAGE_PREV, /* left upper  page button (KEY_PREVIOUS, 412) */
    SEV_MENU,      /* left lower  page button (KEY_MENU, 139) */
    SEV_BACK,      /* right lower page button (KEY_BACK, 158) */
    SEV_HOME,      /* Home button              (KEY_HOME, 102) */
    SEV_POWER,     /* Power button             (KEY_POWER, 116) */

    SEV_TOUCH_X,   /* ABS_X coordinate -> value */
    SEV_TOUCH_Y,   /* ABS_Y coordinate -> value */
    SEV_TOUCH_DOWN,/* BTN_TOUCH 1=down 0=up -> value */
    SEV_FRAME,     /* SYN_REPORT: touch frame boundary */
};

/* Raw event types/codes (linux/input.h values, stable across 2.6.29). */
#define EV_SYN   0x00
#define EV_KEY   0x01
#define EV_ABS   0x03

#define KEY_MENU     139
#define KEY_HOME     102
#define KEY_POWER    116
#define KEY_PREVIOUS 412
#define KEY_NEXT     407
#define KEY_BACK     158

#define ABS_X     0x00
#define ABS_Y     0x01

#define BTN_TOUCH  330
#define SYN_REPORT 0

/*
 * Map a raw input event to a semantic event.
 *
 *   dev        : 0 = event0, 1 = event1, 2 = event2
 *   type, code : raw EV_* / KEY_* / ABS_* / BTN_* values
 *   value      : raw event value (key press/release 1/0, ABS coordinate)
 *   out_value  : [optional] semantic value to pass with the event
 *
 * Returns SEV_* or SEV_NONE for unmapped events.
 */
static inline enum sev_action inputmap_map(int dev, int type, int code,
                                           int value, int *out_value)
{
    if (out_value)
        *out_value = value;

    if (type == EV_KEY) {
        if (dev == 0) {
            if (code == KEY_NEXT)      return SEV_PAGE_NEXT;
            if (code == KEY_PREVIOUS)  return SEV_PAGE_PREV;
            if (code == KEY_MENU)      return SEV_MENU;
            if (code == KEY_BACK)      return SEV_BACK;
        } else if (dev == 1) {
            if (code == KEY_HOME)      return SEV_HOME;
            if (code == KEY_POWER)     return SEV_POWER;
        } else if (dev == 2) {
            if (code == BTN_TOUCH)     return SEV_TOUCH_DOWN;
        }
        return SEV_NONE;
    }

    if (type == EV_ABS) {
        if (dev == 2) {
            if (code == ABS_X)         return SEV_TOUCH_X;
            if (code == ABS_Y)         return SEV_TOUCH_Y;
        }
        return SEV_NONE;
    }

    if (type == EV_SYN) {
        if (code == SYN_REPORT)        return SEV_FRAME;
        return SEV_NONE;
    }

    return SEV_NONE;
}

#endif /* INPUTMAP_H */