#include "mouse.h"
#include "idt.h"
#include "serial.h"

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

static mouse_state_t state = { .x = 0, .y = 0 };
static uint8_t packet[3];
static uint8_t packet_index = 0;

static int32_t screen_w = 80;
static int32_t screen_h = 25;

void mouse_set_bounds(int32_t width, int32_t height) {
    screen_w = width;
    screen_h = height;
    if (state.x >= screen_w) state.x = screen_w - 1;
    if (state.y >= screen_h) state.y = screen_h - 1;
}

static void ps2_wait_input_clear(void) {
    int timeout = 100000;
    while (timeout-- && (inb(PS2_STATUS_PORT) & 0x02));
}
static void ps2_wait_output_full(void) {
    int timeout = 100000;
    while (timeout-- && !(inb(PS2_STATUS_PORT) & 0x01));
}

static void mouse_write(uint8_t val) {
    ps2_wait_input_clear();
    outb(PS2_CMD_PORT, 0xD4);   /* tell controller: next byte goes to mouse */
    ps2_wait_input_clear();
    outb(PS2_DATA_PORT, val);
}

static uint8_t mouse_read(void) {
    ps2_wait_output_full();
    return inb(PS2_DATA_PORT);
}

static void mouse_irq_handler(registers_t *regs) {
    (void)regs;
    uint8_t data = inb(PS2_DATA_PORT);

    /* Byte 0 must have bit 3 set (sync bit) - resync if we lose framing */
    if (packet_index == 0 && !(data & 0x08)) return;

    packet[packet_index++] = data;
    if (packet_index < 3) return;
    packet_index = 0;

    uint8_t flags = packet[0];
    if (flags & 0xC0) return; /* overflow, discard packet */

    int8_t dx = (int8_t)packet[1];
    int8_t dy = (int8_t)packet[2];
    if (flags & 0x10) dx = dx; /* sign already applied via int8_t cast */
    if (flags & 0x20) dy = dy;

    state.dx = dx;
    state.dy = -dy; /* PS/2 Y axis is inverted vs typical screen coords */
    state.x += state.dx;
    state.y += state.dy;
    if (state.x < 0) state.x = 0;
    if (state.y < 0) state.y = 0;
    if (state.x >= screen_w) state.x = screen_w - 1;
    if (state.y >= screen_h) state.y = screen_h - 1;

    state.left   = flags & 0x01;
    state.right  = flags & 0x02;
    state.middle = flags & 0x04;
}

mouse_state_t mouse_get_state(void) {
    return state;
}

void mouse_init(void) {
    /* Enable auxiliary (mouse) device on the 8042 controller */
    ps2_wait_input_clear();
    outb(PS2_CMD_PORT, 0xA8);

    /* Enable IRQ12 in the controller configuration byte */
    ps2_wait_input_clear();
    outb(PS2_CMD_PORT, 0x20);       /* read config byte */
    uint8_t status = mouse_read();
    status |= 0x02;                  /* enable IRQ12 */
    status &= ~0x20;                 /* enable mouse clock */
    ps2_wait_input_clear();
    outb(PS2_CMD_PORT, 0x60);       /* write config byte */
    ps2_wait_input_clear();
    outb(PS2_DATA_PORT, status);

    /* Use default settings, then enable data reporting */
    mouse_write(0xF6);
    mouse_read(); /* ACK */

    mouse_write(0xF4); /* enable streaming packets */
    mouse_read(); /* ACK */

    register_interrupt_handler(44, mouse_irq_handler); /* IRQ12 -> vector 44 */
    irq_clear_mask(2);  /* cascade line on master PIC must be unmasked */
    irq_clear_mask(12);
    serial_write("[ps2] mouse driver initialized\n");
}
