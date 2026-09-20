#ifndef NUGGET_SOCKS_H
#define NUGGET_SOCKS_H
#include "types.h"

/* .socks - our independent executable format for programs that live
   outside the kernel, in their own folder under software/. A .socks file
   is just: a small fixed header, followed by raw machine code+data that
   gets loaded at a fixed address and run directly (we're still in kernel
   mode - no per-process memory protection yet, that's a much bigger
   separate project). Programs never call kernel functions by address
   directly (those addresses shift every time the kernel is rebuilt) -
   instead they go through socks_api_t, a table of function pointers at a
   fixed, known address that the kernel fills in once at boot. This is
   the same idea as a syscall table or a DLL import table: a stable
   contract between the kernel and a program that doesn't care what
   kernel version it's running against, only that the slots mean the
   same thing every time. */

#define SOCKS_MAGIC "SOCK"
#define SOCKS_VERSION 1
#define SOCKS_LOAD_ADDR 0x700000ULL /* where the kernel copies a .socks file's code before running it */
#define SOCKS_API_ADDR  0x6FF000ULL /* fixed address of the socks_api_t table itself */

typedef struct PACKED {
    char magic[4];       /* "SOCK" */
    uint32_t version;
    uint32_t entry_init;   /* offset to void init(socks_ctx_t*) - called once per window when it's created, 0 = unused */
    uint32_t entry_draw;   /* offset to void draw(socks_ctx_t*) - called whenever the window needs redrawing */
    uint32_t entry_key;    /* offset to void handle_key(socks_ctx_t*, char) - 0 = unused */
    uint32_t entry_click;  /* offset to void handle_click(socks_ctx_t*, int32_t x, int32_t y) - 0 = unused */
    uint32_t code_size;    /* bytes of code+data following this header */
    char name[32];         /* display name, e.g. "NOTEPAD" */
} socks_header_t;

/* Per-window scratch state a .socks program gets to keep its own data in -
   deliberately separate from the kernel's own window_t, since a .socks
   program shouldn't need to know anything about our internal window
   manager structure. */
typedef struct PACKED {
    int32_t x, y, w, h;     /* content area (below the title bar), in screen coordinates */
    char path[256];         /* file this window has open, if any - "" if none */
    uint8_t scratch[16384]; /* the app's own private data (e.g. Notepad's text buffer) */
    uint32_t scratch_len;
} socks_ctx_t;

/* The kernel API table - one function pointer per slot, in a fixed order
   that must never change once a .socks program has been built against
   it (new capabilities get added as new slots at the end, never by
   reordering or removing existing ones). */
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

/* Populates the fixed API table - called once at boot, before any .socks
   program can be loaded. */
void socks_api_init(void);

/* Loads a .socks file from the given path, and if it's valid, runs its
   init entry point (if any) and returns true with *out_header pointing
   at the loaded header (fixed at SOCKS_LOAD_ADDR) for later draw/key/click
   calls. Only one .socks program can be loaded at a time right now (it's
   copied to the same fixed address every time) - loading a second one
   overwrites the first, which is fine as long as callers only keep using
   the most recently loaded one. */
bool socks_load(const char *path, socks_header_t **out_header);

static inline void socks_call_init(socks_header_t *h, socks_ctx_t *ctx) {
    if (!h || !h->entry_init) return;
    void (*fn)(socks_ctx_t*) = (void (*)(socks_ctx_t*))(SOCKS_LOAD_ADDR + h->entry_init);
    fn(ctx);
}
static inline void socks_call_draw(socks_header_t *h, socks_ctx_t *ctx) {
    if (!h || !h->entry_draw) return;
    void (*fn)(socks_ctx_t*) = (void (*)(socks_ctx_t*))(SOCKS_LOAD_ADDR + h->entry_draw);
    fn(ctx);
}
static inline void socks_call_key(socks_header_t *h, socks_ctx_t *ctx, char c) {
    if (!h || !h->entry_key) return;
    void (*fn)(socks_ctx_t*, char) = (void (*)(socks_ctx_t*, char))(SOCKS_LOAD_ADDR + h->entry_key);
    fn(ctx, c);
}
static inline void socks_call_click(socks_header_t *h, socks_ctx_t *ctx, int32_t x, int32_t y) {
    if (!h || !h->entry_click) return;
    void (*fn)(socks_ctx_t*, int32_t, int32_t) = (void (*)(socks_ctx_t*, int32_t, int32_t))(SOCKS_LOAD_ADDR + h->entry_click);
    fn(ctx, x, y);
}

#endif
