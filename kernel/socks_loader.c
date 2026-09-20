#include "socks.h"
#include "fb.h"
#include "font.h"
#include "nuggetfs.h"
#include "string.h"
#include "serial.h"

/* The API table lives at a fixed address so .socks programs (built once,
   separately, against this exact layout) can find it without needing to
   know anything about where the kernel itself was loaded. */
static socks_api_t * const api_table = (socks_api_t*)SOCKS_API_ADDR;

/* Thin wrappers - fb.c/font.c/nuggetfs.c's real functions don't all match
   the exact signatures we want to expose (some take more parameters than
   a .socks program should need to care about), so the table points at
   these instead of the raw kernel functions directly. */
static void api_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color);
}
static void api_fill_rect_vgradient(int32_t x, int32_t y, int32_t w, int32_t h,
                                     uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                     uint8_t bot_r, uint8_t bot_g, uint8_t bot_b) {
    fb_fill_rect_vgradient(x, y, w, h, top_r, top_g, top_b, bot_r, bot_g, bot_b);
}
static void api_draw_string(int32_t x, int32_t y, const char *text, uint32_t color, int32_t scale) {
    font_draw_string(x, y, text, color, scale);
}
static int32_t api_string_width(const char *text, int32_t scale) {
    return font_string_width(text, scale);
}
static int32_t api_char_height(int32_t scale) {
    return font_char_height(scale);
}
static uint32_t api_pack_color(uint8_t r, uint8_t g, uint8_t b) {
    return fb_pack_color(r, g, b);
}
static int api_read_file(const char *path, void *buf, uint32_t maxlen) {
    return nfs_read_path(path, buf, maxlen);
}
static int api_write_file(const char *path, const void *data, uint32_t len) {
    return nfs_write_path(path, data, len);
}
static void *api_mem_copy(void *dst, const void *src, uint32_t n) {
    return memcpy(dst, src, n);
}
static void *api_mem_set(void *dst, int val, uint32_t n) {
    return memset(dst, val, n);
}
static uint32_t api_str_len(const char *s) {
    return (uint32_t)strlen(s);
}

void socks_api_init(void) {
    api_table->fill_rect     = api_fill_rect;
    api_table->fill_rect_vgradient = api_fill_rect_vgradient;
    api_table->draw_string   = api_draw_string;
    api_table->string_width  = api_string_width;
    api_table->char_height   = api_char_height;
    api_table->pack_color    = api_pack_color;
    api_table->read_file     = api_read_file;
    api_table->write_file    = api_write_file;
    api_table->mem_copy      = api_mem_copy;
    api_table->mem_set       = api_mem_set;
    api_table->str_len       = api_str_len;
    serial_write("[socks] kernel API table ready\n");
}

bool socks_load(const char *path, socks_header_t **out_header) {
    /* Read the whole file into a scratch buffer first - we need to see
       the header before we know how much of it is actual code, and we
       don't want to load straight onto SOCKS_LOAD_ADDR in case this
       turns out not to be a valid .socks file at all. */
    static uint8_t scratch[512 * 1024]; /* generous cap for a single .socks program */
    int n = nfs_read_path(path, scratch, sizeof(scratch));
    if (n < (int)sizeof(socks_header_t)) {
        serial_write("[socks] file too small or not found: ");
        serial_write(path);
        serial_write("\n");
        return false;
    }

    socks_header_t *hdr = (socks_header_t*)scratch;
    if (memcmp(hdr->magic, SOCKS_MAGIC, 4) != 0) {
        serial_write("[socks] bad magic, not a .socks file: ");
        serial_write(path);
        serial_write("\n");
        return false;
    }
    if (hdr->version != SOCKS_VERSION) {
        serial_write("[socks] unsupported .socks version\n");
        return false;
    }
    uint32_t total_needed = (uint32_t)sizeof(socks_header_t) + hdr->code_size;
    if ((int)total_needed > n) {
        serial_write("[socks] file truncated (code_size larger than what's on disk)\n");
        return false;
    }

    /* Copy the whole thing (header + code) to the fixed load address -
       the header travels with the code so entry-point offsets (which are
       relative to the START of the file) resolve correctly. */
    memcpy((void*)SOCKS_LOAD_ADDR, scratch, total_needed);

    socks_header_t *loaded = (socks_header_t*)SOCKS_LOAD_ADDR;
    *out_header = loaded;

    serial_write("[socks] loaded ");
    serial_write(loaded->name);
    serial_write("\n");
    return true;
}
