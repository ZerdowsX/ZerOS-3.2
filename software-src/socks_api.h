#ifndef NUGGET_SOCKS_API_H
#define NUGGET_SOCKS_API_H

/* This is the ONLY header a .socks program needs. It's self-contained on
   purpose - a program built against this doesn't need any of the
   kernel's own headers, and won't break if kernel-internal structures
   change later. */

typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef uint8_t            bool;
#define true  1
#define false 0
#define NULL ((void*)0)
#define PACKED __attribute__((packed))

#define SOCKS_API_ADDR 0x6FF000ULL

typedef struct PACKED {
    int32_t x, y, w, h;
    char path[256];
    uint8_t scratch[16384];
    uint32_t scratch_len;
} socks_ctx_t;

typedef struct PACKED {
    void (*fill_rect)(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
    void (*fill_rect_vgradient)(int32_t x, int32_t y, int32_t w, int32_t h,
                                 uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                 uint8_t bot_r, uint8_t bot_g, uint8_t bot_b);
    void (*draw_string)(int32_t x, int32_t y, const char *text, uint32_t color, int32_t scale);
    int32_t (*string_width)(const char *text, int32_t scale);
    int32_t (*char_height)(int32_t scale);
    uint32_t (*pack_color)(uint8_t r, uint8_t g, uint8_t b);
    int (*read_file)(const char *path, void *buf, uint32_t maxlen);
    int (*write_file)(const char *path, const void *data, uint32_t len);
    void *(*mem_copy)(void *dst, const void *src, uint32_t n);
    void *(*mem_set)(void *dst, int val, uint32_t n);
    uint32_t (*str_len)(const char *s);
} socks_api_t;

/* The kernel fills this table in at boot - a .socks program just reads
   through it, it never needs to know where the kernel's own functions
   actually live in memory. */
static socks_api_t * const api = (socks_api_t*)SOCKS_API_ADDR;

#endif
