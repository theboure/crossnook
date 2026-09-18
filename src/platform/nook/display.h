/*
 * display.h — Nook Simple Touch framebuffer display abstraction.
 *
 * This module is the ONLY code that talks to /dev/graphics/fb0. Public UI
 * and graphics code never opens or writes the device node directly.
 *
 * Proven behavior (milestones hardware-bringup / freetype-text-rendering):
 *   - 600x800 RGB565 software framebuffer, stride 1200 bytes;
 *   - full-frame flush = lseek(fd, 0, SEEK_SET) + a partial-write-safe
 *     loop writing exactly 960000 bytes;
 *   - deliberately no mmap, no fb ioctls, no EPD sysfs controls, no
 *     partial refresh.
 */
#ifndef CN_PLATFORM_NOOK_DISPLAY_H
#define CN_PLATFORM_NOOK_DISPLAY_H

#define CN_FB_W        600
#define CN_FB_H        800
#define CN_FB_STRIDE   1200                 /* bytes per row (600 * 2) */
#define CN_FB_BYTES    (CN_FB_W * CN_FB_H * 2)   /* 960000 */

typedef struct cn_display cn_display;

/* Open /dev/graphics/fb0 O_WRONLY. Returns NULL on failure (errno set). */
cn_display *cn_display_open(void);

/* Push one full 960000-byte frame from `pixels` (row-major RGB565,
 * stride here equals the canvas row length). Returns CN_FB_BYTES on
 * success, -1 on error. */
int cn_display_flush(cn_display *d, const void *pixels);

void cn_display_close(cn_display *d);

#endif /* CN_PLATFORM_NOOK_DISPLAY_H */