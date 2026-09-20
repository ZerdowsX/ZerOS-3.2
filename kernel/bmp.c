#include "bmp.h"
#include "fb.h"
#include "string.h"

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF); p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int32_t bmp_encode(const uint32_t *canvas, int32_t cw, int32_t ch, uint8_t *out, int32_t out_cap) {
    int32_t row_bytes = cw * 3;
    int32_t padded_row = (row_bytes + 3) & ~3;
    int32_t data_size = padded_row * ch;
    int32_t file_size = 54 + data_size;
    if (file_size > out_cap || cw <= 0 || ch <= 0) return -1;

    memset(out, 0, 54);
    out[0] = 'B'; out[1] = 'M';
    put_u32(out + 2, (uint32_t)file_size);
    put_u32(out + 10, 54);
    put_u32(out + 14, 40);
    put_u32(out + 18, (uint32_t)cw);
    put_u32(out + 22, (uint32_t)ch); /* positive height = bottom-up rows */
    put_u16(out + 26, 1);
    put_u16(out + 28, 24);
    put_u32(out + 34, (uint32_t)data_size);

    uint8_t *dst = out + 54;
    for (int32_t y = 0; y < ch; y++) {
        int32_t src_y = ch - 1 - y; /* BMP rows are bottom-up */
        uint8_t *row = dst + (size_t)y * padded_row;
        for (int32_t x = 0; x < cw; x++) {
            uint8_t r, g, b;
            fb_unpack_color(canvas[src_y * cw + x], &r, &g, &b);
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
    }
    return file_size;
}

bool bmp_decode(const uint8_t *data, int32_t data_len, uint32_t *out_canvas,
                 int32_t max_w, int32_t max_h, int32_t *out_w, int32_t *out_h) {
    if (data_len < 54 || data[0] != 'B' || data[1] != 'M') return false;

    uint32_t data_offset = get_u32(data + 10);
    int32_t width = (int32_t)get_u32(data + 18);
    int32_t height_raw = (int32_t)get_u32(data + 22);
    bool bottom_up = height_raw >= 0;
    int32_t height = bottom_up ? height_raw : -height_raw;
    uint16_t bpp = get_u16(data + 28);
    uint32_t compression = get_u32(data + 30);

    if (width <= 0 || height <= 0 || bpp != 24 || compression != 0) return false;

    int32_t row_bytes = width * 3;
    int32_t padded_row = (row_bytes + 3) & ~3;

    int32_t cw = width < max_w ? width : max_w;
    int32_t ch = height < max_h ? height : max_h;

    for (int32_t y = 0; y < ch; y++) {
        int32_t src_y = bottom_up ? (height - 1 - y) : y;
        const uint8_t *row = data + data_offset + (size_t)src_y * padded_row;
        if ((size_t)(row - data) + (size_t)row_bytes > (size_t)data_len) break;
        for (int32_t x = 0; x < cw; x++) {
            uint8_t b = row[x * 3 + 0];
            uint8_t g = row[x * 3 + 1];
            uint8_t r = row[x * 3 + 2];
            out_canvas[y * cw + x] = fb_pack_color(r, g, b);
        }
    }

    *out_w = cw;
    *out_h = ch;
    return true;
}
