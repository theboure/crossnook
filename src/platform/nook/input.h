/*
 * input.h — semantic input layer for the Nook Simple Touch.
 *
 * Raw evdev details (device nodes event0/1/2, Linux 2.6.29 struct
 * input_event_le layout) and the raw key/abs code mapping live only
 * inside input.c. Consumers see only cn_input_ev semantic events.
 *
 * Devices (measured on the real Nook):
 *   event0  TWL4030 Keypad   page buttons (KEY_NEXT 407, KEY_PREVIOUS 412,
 *                            KEY_MENU 139, KEY_BACK 158)
 *   event1  gpio-keys        KEY_HOME 102, KEY_POWER 116
 *   event2  zForce Touch     ABS_X / ABS_Y / BTN_TOUCH / SYN_REPORT
 *
 * Touch handling: zForce emits ABS_X, ABS_Y, BTN_TOUCH, then SYN_REPORT.
 * A SYN_REPORT completing a down frame starts a gesture (TOUCH_DOWN). The
 * initial down frame's coordinates are provisional (the real device can
 * report a transient coordinate near the opposite edge in the very first
 * and in the release frames) and MUST NOT be treated as an actionable tap.
 * Every later in-contact SYN_REPORT updates the running coordinate;
 * TOUCH_MOVE is emitted when it changes. The SYN_REPORT completing the up
 * frame ends the gesture and delivers TOUCH_UP carrying the RESOLVED
 * coordinate: the last in-contact coordinate before release (falling back
 * to the down frame's coordinate when the gesture never moved — a simple
 * tap). All coordinates are clamped to the known 600x800 screen bounds.
 * Consumers should commit tap actions (markers, button hits) from the
 * TOUCH_UP event. Missing devices are tolerated at open time; events from
 * whatever devices exist are still delivered.
 */
#ifndef CN_PLATFORM_NOOK_INPUT_H
#define CN_PLATFORM_NOOK_INPUT_H

typedef enum cn_input_event {
    CN_INPUT_NONE = 0,
    CN_INPUT_PAGE_PREV,     /* left upper  page button */
    CN_INPUT_PAGE_NEXT,     /* right upper page button */
    CN_INPUT_MENU,          /* left lower  page button */
    CN_INPUT_BACK,          /* right lower page button */
    CN_INPUT_HOME,          /* Home button */
    CN_INPUT_POWER_DOWN,    /* Power button pressed */
    CN_INPUT_POWER_UP,      /* Power button released */
    CN_INPUT_TOUCH_DOWN,    /* touch contact started (coords on x/y) */
    CN_INPUT_TOUCH_MOVE,    /* contact moved while still down */
    CN_INPUT_TOUCH_UP       /* contact released */
} cn_input_event;

typedef struct cn_input_ev {
    cn_input_event type;
    int x;                  /* touch X for TOUCH_* (device pixels) */
    int y;                  /* touch Y for TOUCH_* (device pixels) */
    int raw_dev;            /* 0/1/2: originating event device (diagnostic) */
} cn_input_ev;

typedef struct cn_input cn_input;

/* Open /dev/input/event0..2 nonblocking. Never fails outright: missing
 * devices just yield fewer event sources (poll() then waits forever if
 * none are open). Returns NULL only on allocation failure. */
cn_input *cn_input_open(void);

/* Wait up to timeout_ms for the next semantic event. Returns 1 and fills
 * *out on success, 0 on timeout, -1 on fatal error (or when no input
 * device is open). */
int cn_input_poll(cn_input *in, cn_input_ev *out, int timeout_ms);

/* Number of event devices currently open. */
int cn_input_devices(const cn_input *in);

void cn_input_close(cn_input *in);

#endif /* CN_PLATFORM_NOOK_INPUT_H */