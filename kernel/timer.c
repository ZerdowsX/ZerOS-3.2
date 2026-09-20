#include "timer.h"
#include "idt.h"
#include "serial.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_HZ  1193182

static volatile uint64_t ticks = 0;
static uint32_t timer_hz = 100;

static void timer_callback(registers_t *regs) {
    (void)regs;
    ticks++;
}

void timer_init(uint32_t hz) {
    timer_hz = hz;
    uint32_t divisor = PIT_BASE_HZ / hz;

    outb(PIT_COMMAND, 0x36); // channel 0, lobyte/hibyte, rate generator
    outb(PIT_CHANNEL0, divisor & 0xFF);
    outb(PIT_CHANNEL0, (divisor >> 8) & 0xFF);

    register_interrupt_handler(32, timer_callback); // IRQ0 -> vector 32
    irq_clear_mask(0);
    serial_write("[timer] PIT initialized\n");
}

uint64_t timer_ticks(void) {
    return ticks;
}

uint64_t timer_uptime_ms(void) {
    return (ticks * 1000ULL) / timer_hz;
}

void timer_sleep_ms(uint64_t ms) {
    uint64_t target = ticks + (ms * timer_hz) / 1000;
    while (ticks < target) {
        __asm__ volatile ("sti; hlt");
    }
}
