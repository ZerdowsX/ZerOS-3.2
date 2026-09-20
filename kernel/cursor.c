#include "cursor.h"
#include "fb.h"
#include "serial.h"

#define CURSOR_W 13
#define CURSOR_H 20

/* Custom arrow cursor derived from the user-provided cur.png (background
   removed via its alpha channel at build time). ' ' = transparent,
   'X' = solid black, 'o' = partial-alpha edge pixel (anti-aliasing tone). */
static const char *cursor_bitmap[CURSOR_H] = {
    "o            ",
    "Xo           ",
    "XXo          ",
    "XXXo         ",
    "XXXXo        ",
    "XXXXXo       ",
    "XXXXXXo      ",
    "XXXXXXXo     ",
    "XXXXXXXXo    ",
    "XXXXXXXXXo   ",
    "XXXXXXXXXXo  ",
    "XXXXXXXXXXXo ",
    "XXXXXXXXoooo ",
    "XXXXXXXX     ",
    "XXo XXXXo    ",
    "Xo  oXXXX    ",
    "o   oXXXXo   ",
    "     XXXXo   ",
    "     oXoo    ",
    "             ",
};

static uint32_t saved_bg[CURSOR_W * CURSOR_H];
static int32_t last_x = 0, last_y = 0;
static bool has_saved = false;
static uint32_t color_black, color_white, color_gray;

void cursor_init(void) {
    color_black = fb_pack_color(0, 0, 0);
    color_white = fb_pack_color(255, 255, 255);
    color_gray = fb_pack_color(160, 160, 160);
    has_saved = false;
    serial_write("[cursor] initialized\n");
}

static void restore_background(void) {
    for (int32_t j = 0; j < CURSOR_H; j++) {
        for (int32_t i = 0; i < CURSOR_W; i++) {
            fb_put_pixel(last_x + i, last_y + j, saved_bg[j * CURSOR_W + i]);
        }
    }
}

static void save_background(int32_t x, int32_t y) {
    for (int32_t j = 0; j < CURSOR_H; j++) {
        for (int32_t i = 0; i < CURSOR_W; i++) {
            saved_bg[j * CURSOR_W + i] = fb_get_pixel(x + i, y + j);
        }
    }
}

static void draw_arrow(int32_t x, int32_t y) {
    for (int32_t j = 0; j < CURSOR_H; j++) {
        for (int32_t i = 0; i < CURSOR_W; i++) {
            char c = cursor_bitmap[j][i];
            if (c == 'X') fb_put_pixel(x + i, y + j, color_black);
            else if (c == '.') fb_put_pixel(x + i, y + j, color_white);
            else if (c == 'o') fb_put_pixel(x + i, y + j, color_gray);
            /* space = transparent, leave background alone */
        }
    }
}

void cursor_move_to(int32_t x, int32_t y) {
    if (!fb_available()) return;

    if (has_saved) restore_background();
    save_background(x, y);
    draw_arrow(x, y);

    last_x = x;
    last_y = y;
    has_saved = true;
}

void cursor_reset(int32_t x, int32_t y) {
    if (!fb_available()) return;
    /* Skip restoring - the screen was just fully redrawn, so the old
       snapshot no longer matches what's underneath. Just snapshot fresh
       background at the new position and draw on top of it. */
    save_background(x, y);
    draw_arrow(x, y);
    last_x = x;
    last_y = y;
    has_saved = true;
}
