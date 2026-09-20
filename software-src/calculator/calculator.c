/* calculator.c - standalone .socks port of the built-in Calculator. The
   math logic (fixed-point parse/format/apply) is carried over unchanged
   from the original - it never touched any kernel function directly, so
   it ports over exactly as-is. Only the drawing and click-hit-testing
   needed rewriting against the .socks API. */

#include "../ui.h"

#define CALC_GRID_ROWS 5
#define CALC_GRID_COLS 4
#define CALC_BTN_W 46
#define CALC_BTN_H 30
#define CALC_BTN_GAP 4
#define CALC_SCALE 10000

typedef struct {
    char display[24];
    int64_t accum;
    char pending_op;
    bool fresh_entry;
} calc_state_t;

static const char *calc_labels[CALC_GRID_ROWS][CALC_GRID_COLS] = {
    {"C",  "",   "",   "" },
    {"7",  "8",  "9",  "/"},
    {"4",  "5",  "6",  "*"},
    {"1",  "2",  "3",  "-"},
    {"0",  ".",  "=",  "+"},
};

static void button_rect(socks_ctx_t *ctx, int row, int col, int32_t *bx, int32_t *by) {
    *bx = ctx->x + 6 + col * (CALC_BTN_W + CALC_BTN_GAP);
    *by = ctx->y + 40 + row * (CALC_BTN_H + CALC_BTN_GAP);
}

static bool in_rect(int32_t px, int32_t py, int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void append_digit(calc_state_t *st, char d) {
    if (st->fresh_entry) {
        st->display[0] = d;
        st->display[1] = '\0';
        st->fresh_entry = false;
    } else {
        uint32_t len = api->str_len(st->display);
        if (len < sizeof(st->display) - 1) {
            st->display[len] = d;
            st->display[len + 1] = '\0';
        }
    }
}

static int64_t calc_parse(const char *s) {
    int64_t int_part = 0, frac_part = 0, frac_scale = 1;
    bool in_frac = false, neg = false;
    if (*s == '-') { neg = true; s++; }
    for (; *s; s++) {
        if (*s == '.') { in_frac = true; continue; }
        if (*s < '0' || *s > '9') continue;
        if (in_frac) {
            if (frac_scale < CALC_SCALE) {
                frac_scale *= 10;
                frac_part = frac_part * 10 + (*s - '0');
            }
        } else {
            int_part = int_part * 10 + (*s - '0');
        }
    }
    int64_t scaled = int_part * CALC_SCALE + (frac_part * CALC_SCALE) / frac_scale;
    return neg ? -scaled : scaled;
}

static void calc_format(calc_state_t *st, int64_t scaled) {
    bool neg = scaled < 0;
    if (neg) scaled = -scaled;
    int64_t int_part = scaled / CALC_SCALE;
    int64_t frac_part = scaled % CALC_SCALE;

    char buf[24];
    int i = 0;
    if (neg) buf[i++] = '-';

    char digits[24]; int di = 0;
    if (int_part == 0) digits[di++] = '0';
    while (int_part > 0 && di < 20) {
        digits[di++] = (char)('0' + (int_part % 10));
        int_part /= 10;
    }
    while (di > 0) buf[i++] = digits[--di];

    if (frac_part > 0) {
        buf[i++] = '.';
        char fbuf[5];
        int64_t f = frac_part;
        for (int k = 3; k >= 0; k--) { fbuf[k] = (char)('0' + f % 10); f /= 10; }
        int end = 4;
        while (end > 1 && fbuf[end - 1] == '0') end--;
        for (int k = 0; k < end; k++) buf[i++] = fbuf[k];
    }
    buf[i] = '\0';

    uint32_t n = api->str_len(buf);
    if (n > sizeof(st->display) - 1) n = sizeof(st->display) - 1;
    api->mem_copy(st->display, buf, n);
    st->display[n] = '\0';
}

static int64_t calc_apply(char op, int64_t a, int64_t b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return (a * b) / CALC_SCALE;
        case '/': return (b != 0) ? (a * CALC_SCALE) / b : 0;
        default:  return b;
    }
}

static void handle_button(calc_state_t *st, const char *label) {
    if (label[0] == '\0') return;

    if (label[0] >= '0' && label[0] <= '9') {
        append_digit(st, label[0]);
    } else if (label[0] == '.') {
        if (!st->fresh_entry) {
            bool has_dot = false;
            for (const char *p = st->display; *p; p++) if (*p == '.') has_dot = true;
            if (!has_dot) append_digit(st, '.');
        } else {
            st->display[0] = '0'; st->display[1] = '.'; st->display[2] = '\0';
            st->fresh_entry = false;
        }
    } else if (label[0] == 'C') {
        st->accum = 0;
        st->pending_op = 0;
        st->fresh_entry = true;
        st->display[0] = '0'; st->display[1] = '\0';
    } else if (label[0] == '=') {
        int64_t cur = calc_parse(st->display);
        int64_t result = calc_apply(st->pending_op, st->accum, cur);
        if (!st->pending_op) result = cur;
        calc_format(st, result);
        st->accum = result;
        st->pending_op = 0;
        st->fresh_entry = true;
    } else {
        int64_t cur = calc_parse(st->display);
        if (st->pending_op) {
            int64_t result = calc_apply(st->pending_op, st->accum, cur);
            st->accum = result;
            calc_format(st, result);
        } else {
            st->accum = cur;
        }
        st->pending_op = label[0];
        st->fresh_entry = true;
    }
}

void init(socks_ctx_t *ctx) {
    calc_state_t *st = (calc_state_t*)ctx->scratch;
    st->display[0] = '0'; st->display[1] = '\0';
    st->accum = 0;
    st->pending_op = 0;
    st->fresh_entry = true;
}

void draw(socks_ctx_t *ctx) {
    calc_state_t *st = (calc_state_t*)ctx->scratch;
    uint32_t face = api->pack_color(192, 192, 192);
    uint32_t black = api->pack_color(0, 0, 0);

    api->fill_rect(ctx->x, ctx->y, ctx->w, ctx->h, face);

    ui_field(ctx->x + 6, ctx->y + 6, ctx->w - 12, 28);
    int32_t tw = api->string_width(st->display, 2);
    api->draw_string(ctx->x + ctx->w - 10 - tw, ctx->y + 6 + (28 - api->char_height(2)) / 2,
                      st->display, black, 2);

    for (int row = 0; row < CALC_GRID_ROWS; row++) {
        for (int col = 0; col < CALC_GRID_COLS; col++) {
            const char *label = calc_labels[row][col];
            if (label[0] == '\0') continue;
            int32_t bx, by;
            button_rect(ctx, row, col, &bx, &by);
            ui_button(bx, by, CALC_BTN_W, CALC_BTN_H, false);
            ui_button_label(bx, by, CALC_BTN_W, CALC_BTN_H, label, black, 2);
        }
    }
}

void handle_click(socks_ctx_t *ctx, int32_t x, int32_t y) {
    calc_state_t *st = (calc_state_t*)ctx->scratch;
    for (int row = 0; row < CALC_GRID_ROWS; row++) {
        for (int col = 0; col < CALC_GRID_COLS; col++) {
            const char *label = calc_labels[row][col];
            if (label[0] == '\0') continue;
            int32_t bx, by;
            button_rect(ctx, row, col, &bx, &by);
            if (in_rect(x, y, bx, by, CALC_BTN_W, CALC_BTN_H)) {
                handle_button(st, label);
                return;
            }
        }
    }
}
