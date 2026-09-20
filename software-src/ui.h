#ifndef NUGGET_UI_H
#define NUGGET_UI_H
#include "socks_api.h"

/* ui.h - ready-made interface pieces for .socks programs, so you don't
   have to hand-draw every button and panel from flat rectangles. Built
   entirely on top of socks_api.h's raw primitives - include this instead
   of (or alongside) socks_api.h and call these instead of drawing
   rectangles by hand. Matches the look of the rest of the OS (the same
   glossy green-on-hover / white gradient buttons everywhere else use). */

/* A glossy gradient button with a white outline - the default look for
   ordinary buttons everywhere in the OS (taskbar, toolbars, dialogs).
   Pass pressed=true for a "held down" look (inverted gradient). */
static inline void ui_button(int32_t x, int32_t y, int32_t w, int32_t h, bool pressed) {
    uint32_t white = api->pack_color(255, 255, 255);
    api->fill_rect(x, y, w, 1, white);
    api->fill_rect(x, y, 1, h, white);
    api->fill_rect(x, y + h - 1, w, 1, white);
    api->fill_rect(x + w - 1, y, 1, h, white);
    if (pressed) {
        api->fill_rect_vgradient(x + 1, y + 1, w - 2, h - 2, 195, 195, 200, 255, 255, 255);
    } else {
        api->fill_rect_vgradient(x + 1, y + 1, w - 2, h - 2, 255, 255, 255, 195, 195, 200);
    }
}

/* Same button, but with a custom-colored outline and gradient - use this
   for anything that needs to stand out with its own color (like the
   kernel's own close/minimize/maximize buttons use red/blue). */
static inline void ui_button_colored(int32_t x, int32_t y, int32_t w, int32_t h, bool pressed,
                                      uint32_t outline_color,
                                      uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                      uint8_t bot_r, uint8_t bot_g, uint8_t bot_b) {
    api->fill_rect(x, y, w, 1, outline_color);
    api->fill_rect(x, y, 1, h, outline_color);
    api->fill_rect(x, y + h - 1, w, 1, outline_color);
    api->fill_rect(x + w - 1, y, 1, h, outline_color);
    if (pressed) {
        api->fill_rect_vgradient(x + 1, y + 1, w - 2, h - 2, bot_r, bot_g, bot_b, top_r, top_g, top_b);
    } else {
        api->fill_rect_vgradient(x + 1, y + 1, w - 2, h - 2, top_r, top_g, top_b, bot_r, bot_g, bot_b);
    }
}

/* Draws `label` centered inside a button rect already drawn with
   ui_button/ui_button_colored - a common enough pairing to save the
   centering math every time. */
static inline void ui_button_label(int32_t x, int32_t y, int32_t w, int32_t h,
                                    const char *label, uint32_t color, int32_t scale) {
    int32_t lw = api->string_width(label, scale);
    int32_t lh = api->char_height(scale);
    api->draw_string(x + (w - lw) / 2, y + (h - lh) / 2, label, color, scale);
}

/* The OS's standard green title-bar gradient - use this if your program
   ever draws its own title bar or header strip and wants to match. */
static inline void ui_titlebar_gradient(int32_t x, int32_t y, int32_t w, int32_t h) {
    api->fill_rect_vgradient(x, y, w, h, 130, 190, 110, 35, 90, 40);
}

/* A plain panel/toolbar strip - the flat gray background used behind
   groups of controls (e.g. Notepad's top bar, a dialog's body). */
static inline void ui_panel(int32_t x, int32_t y, int32_t w, int32_t h) {
    api->fill_rect(x, y, w, h, api->pack_color(192, 192, 192));
}

/* A sunken white input/display field with a simple dark top-left border -
   the look used for text fields and number displays. */
static inline void ui_field(int32_t x, int32_t y, int32_t w, int32_t h) {
    uint32_t white = api->pack_color(255, 255, 255);
    uint32_t dark = api->pack_color(90, 90, 90);
    api->fill_rect(x, y, w, h, white);
    api->fill_rect(x, y, w, 1, dark);
    api->fill_rect(x, y, 1, h, dark);
}

#endif
