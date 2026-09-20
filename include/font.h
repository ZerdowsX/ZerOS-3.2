#ifndef NUGGET_FONT_H
#define NUGGET_FONT_H
#include "types.h"

/* Draws a single character at (x, y) using the built-in 5x7 bitmap font,
   each font pixel rendered as a (scale x scale) block of real pixels. */
void font_draw_char(int32_t x, int32_t y, char c, uint32_t color, int32_t scale);
void font_get_glyph_bitmap(char c, uint8_t out[7][5]);

/* Draws a null-terminated string, advancing left to right. */
void font_draw_string(int32_t x, int32_t y, const char *s, uint32_t color, int32_t scale);

/* Returns the pixel width/height a string would occupy at the given scale -
   useful for centering text in buttons/menus. */
int32_t font_string_width(const char *s, int32_t scale);
int32_t font_char_height(int32_t scale);

#endif
