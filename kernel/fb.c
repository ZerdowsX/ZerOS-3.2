#include "fb.h"
#include "multiboot2.h"
#include "serial.h"
#include "heap.h"
#include "string.h"

static uint8_t *fb_addr = NULL;
static uint32_t fb_pitch = 0;
static uint32_t fb_w = 0, fb_h = 0;
static uint8_t  fb_bpp = 0;
static uint8_t  red_pos = 16, red_size = 8;
static uint8_t  green_pos = 8, green_size = 8;
static uint8_t  blue_pos = 0, blue_size = 8;
static bool     available = false;

/* Double buffering: all drawing goes to this off-screen buffer, and
   fb_present() blits the whole thing to the real framebuffer in one go.
   Without this, dragging a window (which redraws the whole screen every
   frame) was visibly tearing/flickering since the real video memory was
   being overwritten piece by piece while still being scanned out. */
static uint8_t *back_buffer = NULL;
static uint32_t back_pitch = 0; /* tightly packed: fb_w * (fb_bpp/8) */

bool fb_available(void) { return available; }
uint32_t fb_width(void)  { return fb_w; }
uint32_t fb_height(void) { return fb_h; }
uint64_t fb_phys_addr(void) { return (uint64_t)fb_addr; }
uint64_t fb_phys_size(void) { return (uint64_t)fb_pitch * fb_h; }

bool fb_init(uint32_t mb2_info_addr) {
    mb2_info_header_t *hdr = (mb2_info_header_t*)(uint64_t)mb2_info_addr;
    uint8_t *tag_ptr = (uint8_t*)hdr + sizeof(mb2_info_header_t);
    uint8_t *end = (uint8_t*)hdr + hdr->total_size;

    while (tag_ptr < end) {
        mb2_tag_t *tag = (mb2_tag_t*)tag_ptr;
        if (tag->type == MB2_TAG_END) break;

        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            mb2_tag_framebuffer_t *fb = (mb2_tag_framebuffer_t*)tag;
            fb_addr = (uint8_t*)(uint64_t)fb->addr;
            fb_pitch = fb->pitch;
            fb_w = fb->width;
            fb_h = fb->height;
            fb_bpp = fb->bpp;

            if (fb->fb_type == 1) { /* RGB direct color - honor actual channel layout */
                red_pos = fb->red_field_position;
                red_size = fb->red_mask_size;
                green_pos = fb->green_field_position;
                green_size = fb->green_mask_size;
                blue_pos = fb->blue_field_position;
                blue_size = fb->blue_mask_size;
            }

            if (fb_bpp >= 24 && fb_w > 0 && fb_h > 0) {
                back_pitch = fb_w * (fb_bpp / 8);
                back_buffer = (uint8_t*)kmalloc((size_t)back_pitch * fb_h);
                if (back_buffer) {
                    memset(back_buffer, 0, (size_t)back_pitch * fb_h);
                    available = true;
                    serial_write("[fb] framebuffer found and usable (double-buffered)\n");
                } else {
                    serial_write("[fb] framebuffer found but back buffer allocation failed\n");
                }
            } else {
                serial_write("[fb] framebuffer tag present but unusable (bpp/type)\n");
            }
            return available;
        }
        tag_ptr += (tag->size + 7) & ~7;
    }

    serial_write("[fb] no framebuffer tag from GRUB\n");
    return false;
}

uint32_t fb_pack_color(uint8_t r, uint8_t g, uint8_t b) {
    uint32_t rv = ((uint32_t)r >> (8 - red_size))   << red_pos;
    uint32_t gv = ((uint32_t)g >> (8 - green_size)) << green_pos;
    uint32_t bv = ((uint32_t)b >> (8 - blue_size))  << blue_pos;
    return rv | gv | bv;
}

void fb_unpack_color(uint32_t color, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint32_t rmask = (red_size   >= 32) ? 0xFFFFFFFFu : ((1u << red_size)   - 1);
    uint32_t gmask = (green_size >= 32) ? 0xFFFFFFFFu : ((1u << green_size) - 1);
    uint32_t bmask = (blue_size  >= 32) ? 0xFFFFFFFFu : ((1u << blue_size)  - 1);
    uint32_t rv = (color >> red_pos)   & rmask;
    uint32_t gv = (color >> green_pos) & gmask;
    uint32_t bv = (color >> blue_pos)  & bmask;
    *r = (uint8_t)(rv << (8 - red_size));
    *g = (uint8_t)(gv << (8 - green_size));
    *b = (uint8_t)(bv << (8 - blue_size));
}

void fb_put_pixel(int32_t x, int32_t y, uint32_t color) {
    if (!available || x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h) return;
    uint8_t *p = back_buffer + (uint32_t)y * back_pitch + (uint32_t)x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        *(uint32_t*)p = color;
    } else { /* 24bpp */
        p[0] = (uint8_t)(color & 0xFF);
        p[1] = (uint8_t)((color >> 8) & 0xFF);
        p[2] = (uint8_t)((color >> 16) & 0xFF);
    }
}

uint32_t fb_get_pixel(int32_t x, int32_t y) {
    if (!available || x < 0 || y < 0 || (uint32_t)x >= fb_w || (uint32_t)y >= fb_h) return 0;
    uint8_t *p = back_buffer + (uint32_t)y * back_pitch + (uint32_t)x * (fb_bpp / 8);
    if (fb_bpp == 32) {
        return *(uint32_t*)p;
    }
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

void fb_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!available) return;
    int32_t x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int32_t)fb_w) x1 = (int32_t)fb_w;
    if (y1 > (int32_t)fb_h) y1 = (int32_t)fb_h;
    if (x0 >= x1 || y0 >= y1) return;

    int32_t row_w = x1 - x0;
    if (fb_bpp == 32) {
        for (int32_t j = y0; j < y1; j++) {
            uint32_t *row = (uint32_t*)(back_buffer + (size_t)j * back_pitch + (size_t)x0 * 4);
            for (int32_t i = 0; i < row_w; i++) row[i] = color;
        }
    } else {
        uint8_t b0 = (uint8_t)(color & 0xFF), b1 = (uint8_t)((color >> 8) & 0xFF), b2 = (uint8_t)((color >> 16) & 0xFF);
        for (int32_t j = y0; j < y1; j++) {
            uint8_t *row = back_buffer + (size_t)j * back_pitch + (size_t)x0 * 3;
            for (int32_t i = 0; i < row_w; i++) {
                row[i * 3 + 0] = b0; row[i * 3 + 1] = b1; row[i * 3 + 2] = b2;
            }
        }
    }
}

void fb_blit_rgba(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *rgba_data, int32_t src_w) {
    if (!available) return;
    int32_t x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    int32_t src_x_off = 0, src_y_off = 0;
    if (x0 < 0) { src_x_off = -x0; x0 = 0; }
    if (y0 < 0) { src_y_off = -y0; y0 = 0; }
    if (x1 > (int32_t)fb_w) x1 = (int32_t)fb_w;
    if (y1 > (int32_t)fb_h) y1 = (int32_t)fb_h;
    if (x0 >= x1 || y0 >= y1) return;

    for (int32_t j = y0; j < y1; j++) {
        const uint8_t *src = rgba_data + (size_t)(j - y0 + src_y_off) * src_w * 4 + (size_t)src_x_off * 4;
        for (int32_t i = x0; i < x1; i++) {
            uint8_t a = src[3];
            if (a == 0) { src += 4; continue; }
            if (a == 255) {
                fb_put_pixel(i, j, fb_pack_color(src[0], src[1], src[2]));
            } else {
                uint32_t dest = fb_get_pixel(i, j);
                uint8_t dr, dg, db;
                fb_unpack_color(dest, &dr, &dg, &db);
                uint8_t r = (uint8_t)((src[0] * a + dr * (255 - a)) / 255);
                uint8_t g = (uint8_t)((src[1] * a + dg * (255 - a)) / 255);
                uint8_t b = (uint8_t)((src[2] * a + db * (255 - a)) / 255);
                fb_put_pixel(i, j, fb_pack_color(r, g, b));
            }
            src += 4;
        }
    }
}

void fb_fill_rect_vgradient(int32_t x, int32_t y, int32_t w, int32_t h,
                             uint8_t top_r, uint8_t top_g, uint8_t top_b,
                             uint8_t bot_r, uint8_t bot_g, uint8_t bot_b) {
    if (!available || h <= 0) return;
    for (int32_t row = 0; row < h; row++) {
        int32_t t_num = row, t_den = (h > 1) ? (h - 1) : 1;
        uint8_t r = (uint8_t)(top_r + ((int32_t)bot_r - top_r) * t_num / t_den);
        uint8_t g = (uint8_t)(top_g + ((int32_t)bot_g - top_g) * t_num / t_den);
        uint8_t b = (uint8_t)(top_b + ((int32_t)bot_b - top_b) * t_num / t_den);
        fb_fill_rect(x, y + row, w, 1, fb_pack_color(r, g, b));
    }
}

void fb_clear(uint32_t color) {
    fb_fill_rect(0, 0, (int32_t)fb_w, (int32_t)fb_h, color);
}

void fb_blit_rgb888(int32_t x, int32_t y, int32_t w, int32_t h, const uint8_t *rgb_data, int32_t src_stride) {
    if (!available) return;
    int32_t x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    int32_t src_x_off = 0, src_y_off = 0;
    if (x0 < 0) { src_x_off = -x0; x0 = 0; }
    if (y0 < 0) { src_y_off = -y0; y0 = 0; }
    if (x1 > (int32_t)fb_w) x1 = (int32_t)fb_w;
    if (y1 > (int32_t)fb_h) y1 = (int32_t)fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    int32_t row_w = x1 - x0;

    for (int32_t j = y0; j < y1; j++) {
        const uint8_t *src = rgb_data + (size_t)(j - y0 + src_y_off) * src_stride + (size_t)src_x_off * 3;
        if (fb_bpp == 32) {
            uint32_t *dst = (uint32_t*)(back_buffer + (size_t)j * back_pitch + (size_t)x0 * 4);
            for (int32_t i = 0; i < row_w; i++) {
                dst[i] = fb_pack_color(src[i * 3], src[i * 3 + 1], src[i * 3 + 2]);
            }
        } else {
            uint8_t *dst = back_buffer + (size_t)j * back_pitch + (size_t)x0 * 3;
            for (int32_t i = 0; i < row_w; i++) {
                uint32_t packed = fb_pack_color(src[i * 3], src[i * 3 + 1], src[i * 3 + 2]);
                dst[i * 3 + 0] = (uint8_t)(packed & 0xFF);
                dst[i * 3 + 1] = (uint8_t)((packed >> 8) & 0xFF);
                dst[i * 3 + 2] = (uint8_t)((packed >> 16) & 0xFF);
            }
        }
    }
}

void fb_blit_packed(int32_t x, int32_t y, int32_t w, int32_t h, const uint32_t *packed_data, int32_t src_w) {
    if (!available) return;
    int32_t x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    int32_t src_x_off = 0, src_y_off = 0;
    if (x0 < 0) { src_x_off = -x0; x0 = 0; }
    if (y0 < 0) { src_y_off = -y0; y0 = 0; }
    if (x1 > (int32_t)fb_w) x1 = (int32_t)fb_w;
    if (y1 > (int32_t)fb_h) y1 = (int32_t)fb_h;
    if (x0 >= x1 || y0 >= y1) return;
    int32_t row_w = x1 - x0;

    for (int32_t j = y0; j < y1; j++) {
        const uint32_t *src = packed_data + (size_t)(j - y0 + src_y_off) * src_w + src_x_off;
        if (fb_bpp == 32) {
            /* Packed data is already in this exact device format - a straight memcpy per row. */
            memcpy(back_buffer + (size_t)j * back_pitch + (size_t)x0 * 4, src, (size_t)row_w * 4);
        } else {
            uint8_t *dst = back_buffer + (size_t)j * back_pitch + (size_t)x0 * 3;
            for (int32_t i = 0; i < row_w; i++) {
                dst[i * 3 + 0] = (uint8_t)(src[i] & 0xFF);
                dst[i * 3 + 1] = (uint8_t)((src[i] >> 8) & 0xFF);
                dst[i * 3 + 2] = (uint8_t)((src[i] >> 16) & 0xFF);
            }
        }
    }
}

void fb_present(void) {
    if (!available) return;
    if (back_pitch == fb_pitch) {
        /* Common case: hardware pitch already matches our tight packing - one big copy. */
        memcpy(fb_addr, back_buffer, (size_t)back_pitch * fb_h);
    } else {
        for (uint32_t y = 0; y < fb_h; y++) {
            memcpy(fb_addr + (size_t)y * fb_pitch, back_buffer + (size_t)y * back_pitch, back_pitch);
        }
    }
}

void fb_present_rows(int32_t y0, int32_t y1) {
    if (!available) return;
    if (y0 < 0) y0 = 0;
    if (y1 > (int32_t)fb_h) y1 = (int32_t)fb_h;
    if (y0 >= y1) return;
    for (int32_t y = y0; y < y1; y++) {
        memcpy(fb_addr + (size_t)y * fb_pitch, back_buffer + (size_t)y * back_pitch, back_pitch);
    }
}
