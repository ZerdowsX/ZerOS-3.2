#include "wallpaper.h"
#include "fb.h"
#include "types.h"
#include "heap.h"
#include "bmp.h"
#include "nuggetfs.h"
#include "string.h"

#define WALLPAPER_W 1024
#define WALLPAPER_H 768

extern const uint8_t wallpaper_data[]; /* raw RGB888 pixels, WALLPAPER_W*WALLPAPER_H*3 bytes (built-in default) */

/* If set, a user-chosen .bmp takes over instead of the built-in default. */
static uint32_t *custom_wallpaper = NULL;
static int32_t custom_w = 0, custom_h = 0;

/* The built-in wallpaper never changes, but re-packing its colors (a
   shift/mask per pixel) on every single frame was a real, measured cost -
   about 40ms for a 1024x768 image, entirely wasted since the result is
   always identical. Pack it once, lazily, into device-native pixels and
   just memcpy it every frame after that. */
static uint32_t *default_wallpaper_cache = NULL;
static int32_t default_cache_w = 0, default_cache_h = 0;

void wallpaper_draw(void) {
    wallpaper_draw_rows(0, (int32_t)fb_height());
}

void wallpaper_draw_rows(int32_t y0, int32_t y1) {
    if (!fb_available()) return;

    int32_t w = (int32_t)fb_width();
    int32_t h = (int32_t)fb_height();
    if (y0 < 0) y0 = 0;
    if (y1 > h) y1 = h;
    if (y0 >= y1) return;
    int32_t rows = y1 - y0;

    if (custom_wallpaper) {
        int32_t blit_h = (y1 <= custom_h) ? rows : (custom_h > y0 ? custom_h - y0 : 0);
        if (blit_h > 0) {
            fb_blit_packed(0, y0, custom_w, blit_h, custom_wallpaper + (size_t)y0 * custom_w, custom_w);
        }
        return;
    }

    if (w > WALLPAPER_W) w = WALLPAPER_W;
    if (h > WALLPAPER_H) h = WALLPAPER_H;

    if (!default_wallpaper_cache) {
        default_wallpaper_cache = (uint32_t*)kmalloc((size_t)w * h * sizeof(uint32_t));
        if (default_wallpaper_cache) {
            for (int32_t y = 0; y < h; y++) {
                const uint8_t *row = wallpaper_data + (size_t)y * WALLPAPER_W * 3;
                uint32_t *out_row = default_wallpaper_cache + (size_t)y * w;
                for (int32_t x = 0; x < w; x++) {
                    out_row[x] = fb_pack_color(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
                }
            }
            default_cache_w = w;
            default_cache_h = h;
        }
    }

    if (default_wallpaper_cache) {
        int32_t clipped_y1 = y1 < default_cache_h ? y1 : default_cache_h;
        int32_t blit_h = clipped_y1 - y0;
        if (blit_h > 0) {
            fb_blit_packed(0, y0, default_cache_w, blit_h,
                            default_wallpaper_cache + (size_t)y0 * default_cache_w, default_cache_w);
        }
    } else {
        /* Allocation failed for some reason - fall back to the slower direct path rather than nothing. */
        fb_blit_rgb888(0, y0, w, rows, wallpaper_data + (size_t)y0 * WALLPAPER_W * 3, WALLPAPER_W * 3);
    }
}

bool wallpaper_set_from_file(const char *path) {
    static uint8_t bmp_buf[NFS_MAX_FILE_SIZE];
    int n = nfs_read_path(path, bmp_buf, sizeof(bmp_buf));
    if (n <= 0) return false;

    int32_t sw = (int32_t)fb_width(), sh = (int32_t)fb_height();
    uint32_t *new_buf = (uint32_t*)kmalloc((size_t)sw * sh * sizeof(uint32_t));
    if (!new_buf) return false;

    int32_t decoded_w, decoded_h;
    if (!bmp_decode(bmp_buf, n, new_buf, sw, sh, &decoded_w, &decoded_h)) {
        kfree(new_buf);
        return false;
    }

    if (custom_wallpaper) kfree(custom_wallpaper);
    custom_wallpaper = new_buf;
    custom_w = decoded_w;
    custom_h = decoded_h;

    nfs_write_path("wallpaper.cfg", path, (uint32_t)strlen(path));
    return true;
}

void wallpaper_restore_saved_choice(void) {
    char path[160];
    int n = nfs_read_path("wallpaper.cfg", path, sizeof(path) - 1);
    if (n <= 0) return;
    path[n] = '\0';
    wallpaper_set_from_file(path);
}
