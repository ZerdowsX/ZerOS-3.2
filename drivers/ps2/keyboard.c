#include "keyboard.h"
#include "idt.h"
#include "serial.h"

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

#define KBD_BUFFER_SIZE 256
static char kbd_buffer[KBD_BUFFER_SIZE];
static volatile uint32_t kbd_head = 0;
static volatile uint32_t kbd_tail = 0;

static bool shift_pressed = false;
static bool caps_lock = false;

/* Scancode set 1 -> ASCII (unshifted), US QWERTY layout */
static const char scancode_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, 'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,'\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
    /* rest unused for now (F-keys, numpad, etc.) */
};

static const char scancode_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0, 'A','S','D','F','G','H','J','K','L',':','"','~',
    0, '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

#define SC_LSHIFT       0x2A
#define SC_RSHIFT       0x36
#define SC_LSHIFT_REL   0xAA
#define SC_RSHIFT_REL   0xB6
#define SC_CAPSLOCK     0x3A

static void kbd_buffer_push(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUFFER_SIZE;
    if (next != kbd_tail) { /* drop char if buffer full */
        kbd_buffer[kbd_head] = c;
        kbd_head = next;
    }
}

static void keyboard_irq_handler(registers_t *regs) {
    (void)regs;
    uint8_t scancode = inb(PS2_DATA_PORT);

    if (scancode == SC_LSHIFT || scancode == SC_RSHIFT) { shift_pressed = true; return; }
    if (scancode == SC_LSHIFT_REL || scancode == SC_RSHIFT_REL) { shift_pressed = false; return; }
    if (scancode == SC_CAPSLOCK) { caps_lock = !caps_lock; return; }

    if (scancode & 0x80) return; /* key release, ignore for now */

    if (scancode < 128) {
        bool upper = shift_pressed ^ caps_lock;
        char c = upper ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
        if (c) kbd_buffer_push(c);
    }
}

bool keyboard_has_data(void) {
    return kbd_head != kbd_tail;
}

char keyboard_getchar(void) {
    if (kbd_head == kbd_tail) return 0;
    char c = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUFFER_SIZE;
    return c;
}

void keyboard_init(void) {
    /* Flush any pending output byte */
    while (inb(PS2_STATUS_PORT) & 1) inb(PS2_DATA_PORT);

    register_interrupt_handler(33, keyboard_irq_handler); /* IRQ1 -> vector 33 */
    irq_clear_mask(1);
    serial_write("[ps2] keyboard driver initialized\n");
}
