#include "types.h"
#include "vga.h"
#include "string.h"

static volatile uint16_t *const VGA_MEM = (uint16_t*)0xB8000;
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static size_t vga_row = 0;
static size_t vga_col = 0;
static uint8_t vga_color = 0x0F; // white on black

static inline uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | ((uint16_t)color << 8);
}

void vga_clear(void) {
    for (size_t y = 0; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEM[y * VGA_WIDTH + x] = vga_entry(' ', vga_color);
    vga_row = 0;
    vga_col = 0;
}

void vga_set_color(uint8_t fg, uint8_t bg) {
    vga_color = fg | (bg << 4);
}

static void vga_scroll(void) {
    for (size_t y = 1; y < VGA_HEIGHT; y++)
        for (size_t x = 0; x < VGA_WIDTH; x++)
            VGA_MEM[(y-1) * VGA_WIDTH + x] = VGA_MEM[y * VGA_WIDTH + x];
    for (size_t x = 0; x < VGA_WIDTH; x++)
        VGA_MEM[(VGA_HEIGHT-1) * VGA_WIDTH + x] = vga_entry(' ', vga_color);
    vga_row = VGA_HEIGHT - 1;
}

void vga_putc(char c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else {
        VGA_MEM[vga_row * VGA_WIDTH + vga_col] = vga_entry(c, vga_color);
        vga_col++;
        if (vga_col >= VGA_WIDTH) { vga_col = 0; vga_row++; }
    }
    if (vga_row >= VGA_HEIGHT) vga_scroll();
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

static void vga_write_hex(uint64_t val) {
    char buf[17];
    const char *hex = "0123456789ABCDEF";
    buf[16] = '\0';
    for (int i = 15; i >= 0; i--) {
        buf[i] = hex[val & 0xF];
        val >>= 4;
    }
    vga_write(buf);
}

static void vga_write_dec(uint64_t val) {
    char buf[21];
    int i = 20;
    buf[i--] = '\0';
    if (val == 0) { vga_putc('0'); return; }
    while (val > 0) {
        buf[i--] = '0' + (val % 10);
        val /= 10;
    }
    vga_write(&buf[i+1]);
}

/* Minimal printf-style formatter: supports %s %d %u %x %c %% */
void kprintf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { vga_putc(*p); continue; }
        p++;
        switch (*p) {
            case 's': {
                const char *s = __builtin_va_arg(args, const char*);
                vga_write(s ? s : "(null)");
                break;
            }
            case 'd': {
                int64_t v = __builtin_va_arg(args, int64_t);
                if (v < 0) { vga_putc('-'); v = -v; }
                vga_write_dec((uint64_t)v);
                break;
            }
            case 'u': {
                uint64_t v = __builtin_va_arg(args, uint64_t);
                vga_write_dec(v);
                break;
            }
            case 'x': {
                uint64_t v = __builtin_va_arg(args, uint64_t);
                vga_write_hex(v);
                break;
            }
            case 'c': {
                int v = __builtin_va_arg(args, int);
                vga_putc((char)v);
                break;
            }
            case '%':
                vga_putc('%');
                break;
            default:
                vga_putc('%');
                vga_putc(*p);
        }
    }
    __builtin_va_end(args);
}
