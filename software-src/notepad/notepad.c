/* notepad.c - a standalone .socks program, built completely separately
   from the kernel. It never calls kernel functions directly - everything
   it needs comes through the socks_api_t table at a fixed address, so
   this same compiled notepad.socks keeps working even if the kernel
   itself gets rebuilt and its internal function addresses all shift
   around.

   This is a proof-of-concept port, not a byte-for-byte feature match
   with the built-in Notepad (no Save As, no rename support yet) - it
   proves the .socks format and loader work end to end: independent file,
   its own code, drawing into a window and responding to keystrokes. */

#include "../socks_api.h"

#define TEXT_MAX 16000
#define LINE_H 12
#define TEXT_TOP 30

/* Everything this program needs to remember lives in ctx->scratch,
   reinterpreted as our own state struct - the kernel's window manager
   doesn't know or care what's inside it. */
typedef struct {
    char text[TEXT_MAX];
    uint32_t text_len;
} notepad_state_t;

void init(socks_ctx_t *ctx) {
    notepad_state_t *st = (notepad_state_t*)ctx->scratch;
    st->text_len = 0;
    st->text[0] = '\0';

    if (ctx->path[0]) {
        int n = api->read_file(ctx->path, st->text, TEXT_MAX - 1);
        if (n > 0) {
            st->text_len = (uint32_t)n;
            st->text[n] = '\0';
        }
    }
}

void draw(socks_ctx_t *ctx) {
    uint32_t white = api->pack_color(255, 255, 255);
    uint32_t black = api->pack_color(0, 0, 0);
    uint32_t gray  = api->pack_color(192, 192, 192);

    api->fill_rect(ctx->x, ctx->y, ctx->w, ctx->h, white);
    api->fill_rect(ctx->x, ctx->y, ctx->w, 22, gray);
    api->draw_string(ctx->x + 4, ctx->y + 4, "SOCKS NOTEPAD - AUTOSAVES AS YOU TYPE", black, 1);

    notepad_state_t *st = (notepad_state_t*)ctx->scratch;

    int32_t line_x = ctx->x + 4;
    int32_t line_y = ctx->y + TEXT_TOP;
    char line[128];
    uint32_t line_len = 0;

    for (uint32_t i = 0; i <= st->text_len; i++) {
        char c = (i < st->text_len) ? st->text[i] : '\0';
        bool flush = (c == '\n' || c == '\0' || line_len >= sizeof(line) - 1);
        if (flush) {
            line[line_len] = '\0';
            api->draw_string(line_x, line_y, line, black, 1);
            line_y += LINE_H;
            line_len = 0;
            if (c == '\0') break;
        } else {
            line[line_len++] = c;
        }
    }
}

void handle_key(socks_ctx_t *ctx, char c) {
    notepad_state_t *st = (notepad_state_t*)ctx->scratch;

    if (c == '\b') {
        if (st->text_len > 0) st->text_len--;
        st->text[st->text_len] = '\0';
    } else if (st->text_len < TEXT_MAX - 1) {
        st->text[st->text_len++] = c;
        st->text[st->text_len] = '\0';
    }

    if (ctx->path[0]) {
        api->write_file(ctx->path, st->text, st->text_len);
    }
}
