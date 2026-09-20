#ifndef NUGGET_FB_H
#define NUGGET_FB_H
#include "types.h"

bool fb_init(uint32_t mb2_info_addr);
bool fb_available(void);

uint32_t fb_width(void);
uint32_t fb_height(void);
uint64_t fb_phys_addr(void);
uint64_t fb_phys_size(void);

uint32_t fb_pack_color(uint8_t r, uint8_t g, uint8_t b);
void fb_unpack_color(uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b);

void fb_put_pixel(int32_t x, int32_t y, uint32_t color);
uint32_t fb_get_pixel(int32_t x, int32_t y);
void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
/* Vertical gradient fill (top color to bottom color) - the basis for the
   glossy 3D look on the taskbar, window title bars, and the boot progress
   bar. Cheap: just a per-row solid fill with an interpolated color. */
void fb_fill_rect_vgradient(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint8_t top_r, uint8_t top_g, uint8_t top_b,
                             uint8_t bot_r, uint8_t bot_g, uint8_t bot_b);
void fb_clear(uint32_t color);

/* Fast blit for raw RGB888 source data (3 bytes/pixel, e.g. the embedded
   wallpaper image) - packs colors per row directly into the back buffer
   instead of going through fb_put_pixel per pixel. src_stride is the
   number of bytes between rows in rgb_data (usually w*3, but the source
   image may be wider than the region being blitted). */
void fb_blit_rgb888(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *rgb_data, int32_t src_stride);

/* Fast blit for a buffer already holding device-native packed pixels (as
   produced by fb_pack_color) - e.g. a Paint canvas. Uses memcpy per row
   when the framebuffer is 32bpp, since the format already matches
   exactly. src_w is the source buffer's width in pixels (its stride). */
void fb_blit_packed(int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t *packed_data, int32_t src_w);
/* Alpha-blended blit for RGBA source data (4 bytes/pixel) - used for the
   real, anti-aliased icon set, which has genuine partial transparency at
   its edges (unlike our old flat-color icons which just skipped 'blank'
   pixels entirely). Blends per-pixel against whatever's already there. */
void fb_blit_rgba(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *rgba_data, int32_t src_w);

/* Blits the entire off-screen back buffer to the real framebuffer in one
   pass. Call this once after finishing a frame's worth of drawing - never
   partially, or you're back to visible tearing. */
void fb_present(void);
/* Same, but only copies rows [y0,y1) - used when only part of the screen
   actually changed (e.g. dragging a window), so we don't pay for copying
   the whole framebuffer when most of it is unchanged from last frame. */
void fb_present_rows(int32_t y0, int32_t y1);

#endif
