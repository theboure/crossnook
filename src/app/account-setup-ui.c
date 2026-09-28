#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/account_setup_ui.h"
#include "graphics/canvas.h"
#include "graphics/text.h"
#include "platform/nook/display.h"
#include "platform/nook/input.h"

static int parse_unsigned(const char *s, unsigned *out)
{
    char *end;
    unsigned long v;
    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || !s[0] || *end || v > 0xffffffffUL) return 0;
    *out = (unsigned)v;
    return 1;
}

int main(int argc, char **argv)
{
    cn_account_setup_ui app;
    cn_account_setup_ui_config config;
    cn_display *display;
    cn_canvas *canvas;
    cn_text *text;
    cn_input *input;
    cn_ui *ui;
    cn_input_ev event;
    unsigned major, minor, dns_port, sntp_port;
    if (argc != 11 || !parse_unsigned(argv[4], &major) ||
        !parse_unsigned(argv[5], &minor) || !parse_unsigned(argv[7], &dns_port) ||
        !parse_unsigned(argv[9], &sntp_port)) {
        fprintf(stderr, "usage: account-setup-ui <font> <mountpoint> <root> <major> <minor> <dns-ip> <dns-port> <sntp-ip> <sntp-port> <ca-file>\n");
        return 2;
    }
    memset(&config, 0, sizeof config);
    config.mountpoint = argv[2]; config.root = argv[3];
    config.expected_major = major; config.expected_minor = minor;
    config.dns_ip = argv[6]; config.dns_port = dns_port;
    config.sntp_ip = argv[8]; config.sntp_port = sntp_port;
    config.ca_path = argv[10];
    if (cn_account_setup_ui_init(&app, &config) != 0) {
        fprintf(stderr, "ACCOUNT UI storage=unverified persistence=not-attempted\n");
        return 1;
    }
    text = cn_text_load(argv[1]);
    display = cn_display_open(); input = cn_input_open();
    canvas = cn_canvas_create(CN_FB_W, CN_FB_H); ui = cn_ui_init();
    if (!text || !display || !input || !canvas || !ui) return 1;
    cn_ui_render(ui, canvas, text);
    if (cn_display_flush(display, cn_canvas_pixels(canvas)) != CN_FB_BYTES) return 1;
    for (;;) {
        int rc = cn_input_poll(input, &event, -1);
        int redraw;
        if (rc < 0) break;
        if (!rc) continue;
        redraw = cn_ui_handle(ui, &event);
        if (cn_ui_account_action_pending(ui)) {
            cn_ui_render(ui, canvas, text);
            if (cn_display_flush(display, cn_canvas_pixels(canvas)) != CN_FB_BYTES) break;
            if (cn_account_setup_ui_process(&app, ui) < 0) break;
            redraw = 1;
        }
        if (redraw) {
            cn_ui_render(ui, canvas, text);
            if (cn_display_flush(display, cn_canvas_pixels(canvas)) != CN_FB_BYTES) break;
        }
        if (cn_ui_exit_requested(ui)) break;
    }
    cn_ui_free(ui); cn_canvas_free(canvas); cn_input_close(input);
    cn_display_close(display); cn_text_free(text);
    return 0;
}
