#include "splash.h"
#include "fb.h"
#include "types.h"

#define LOGO_W 400
#define LOGO_H 400
#define PROGRESS_BAR_W 300
#define PROGRESS_BAR_H 18

extern const uint8_t logo_data[];   /* raw RGB888 pixels, LOGO_W*LOGO_H*3 bytes */
extern const uint8_t logo_data_end[];

static int32_t bar_x, bar_y;

void splash_show(void) {
    if (!fb_available()) return;

    fb_clear(fb_pack_color(0, 0, 0)); /* black background */

    int32_t ox = ((int32_t)fb_width() - LOGO_W) / 2;
    int32_t oy = ((int32_t)fb_height() - LOGO_H) / 2;

    const uint8_t *p = logo_data;
    for (int32_t y = 0; y < LOGO_H; y++) {
        for (int32_t x = 0; x < LOGO_W; x++) {
            uint8_t r = p[0], g = p[1], b = p[2];
            p += 3;
            fb_put_pixel(ox + x, oy + y, fb_pack_color(r, g, b));
        }
    }

    bar_x = ((int32_t)fb_width() - PROGRESS_BAR_W) / 2;
    bar_y = oy + LOGO_H + 24;
    splash_set_progress(0);
}

void splash_set_progress(int32_t percent) {
    if (!fb_available()) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    /* Track (empty background) */
    fb_fill_rect(bar_x - 2, bar_y - 2, PROGRESS_BAR_W + 4, PROGRESS_BAR_H + 4, fb_pack_color(50, 50, 50));
    fb_fill_rect(bar_x, bar_y, PROGRESS_BAR_W, PROGRESS_BAR_H, fb_pack_color(15, 15, 15));

    int32_t fill_w = PROGRESS_BAR_W * percent / 100;
    if (fill_w > 0) {
        /* Green glossy gradient: light at top, dark at bottom */
        fb_fill_rect_vgradient(bar_x, bar_y, fill_w, PROGRESS_BAR_H,
                               140, 230, 120,   /* top: light green */
                               20, 110, 30);    /* bottom: dark green */
    }
    fb_present();
}

void splash_clear(void) {
    if (!fb_available()) return;
    fb_clear(fb_pack_color(0, 0, 0));
    fb_present();
}
