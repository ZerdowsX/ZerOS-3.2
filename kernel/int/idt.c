#include "idt.h"
#include "vga.h"
#include "serial.h"
#include "string.h"

typedef struct PACKED {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} idt_entry_t;

typedef struct PACKED {
    uint16_t limit;
    uint64_t base;
} idt_ptr_t;

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

static isr_handler_t interrupt_handlers[256];

/* Declared in isr.asm - one label per vector */
#define DECL_ISR(n) extern void isr##n(void);
DECL_ISR(0) DECL_ISR(1) DECL_ISR(2) DECL_ISR(3) DECL_ISR(4) DECL_ISR(5)
DECL_ISR(6) DECL_ISR(7) DECL_ISR(8) DECL_ISR(9) DECL_ISR(10) DECL_ISR(11)
DECL_ISR(12) DECL_ISR(13) DECL_ISR(14) DECL_ISR(15) DECL_ISR(16) DECL_ISR(17)
DECL_ISR(18) DECL_ISR(19) DECL_ISR(20) DECL_ISR(21) DECL_ISR(22) DECL_ISR(23)
DECL_ISR(24) DECL_ISR(25) DECL_ISR(26) DECL_ISR(27) DECL_ISR(28) DECL_ISR(29)
DECL_ISR(30) DECL_ISR(31)

#define DECL_IRQ(n) extern void irq##n(void);
DECL_IRQ(0) DECL_IRQ(1) DECL_IRQ(2) DECL_IRQ(3) DECL_IRQ(4) DECL_IRQ(5)
DECL_IRQ(6) DECL_IRQ(7) DECL_IRQ(8) DECL_IRQ(9) DECL_IRQ(10) DECL_IRQ(11)
DECL_IRQ(12) DECL_IRQ(13) DECL_IRQ(14) DECL_IRQ(15)

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags) {
    idt[num].offset_low  = handler & 0xFFFF;
    idt[num].offset_mid   = (handler >> 16) & 0xFFFF;
    idt[num].offset_high  = (handler >> 32) & 0xFFFFFFFF;
    idt[num].selector    = selector;
    idt[num].ist         = 0;
    idt[num].type_attr   = flags;
    idt[num].zero        = 0;
}

static void pic_remap(void) {
    uint8_t m1 = inb(0x21), m2 = inb(0xA1);
    outb(0x20, 0x11); io_wait();
    outb(0xA0, 0x11); io_wait();
    outb(0x21, 0x20); io_wait();   // master offset = 32
    outb(0xA1, 0x28); io_wait();   // slave offset  = 40
    outb(0x21, 0x04); io_wait();
    outb(0xA1, 0x02); io_wait();
    outb(0x21, 0x01); io_wait();
    outb(0xA1, 0x01); io_wait();
    outb(0x21, m1);
    outb(0xA1, m2);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void irq_set_mask(uint8_t irq_line) {
    uint16_t port = irq_line < 8 ? 0x21 : 0xA1;
    uint8_t line = irq_line < 8 ? irq_line : irq_line - 8;
    outb(port, inb(port) | (1 << line));
}

void irq_clear_mask(uint8_t irq_line) {
    uint16_t port = irq_line < 8 ? 0x21 : 0xA1;
    uint8_t line = irq_line < 8 ? irq_line : irq_line - 8;
    outb(port, inb(port) & ~(1 << line));
}

void register_interrupt_handler(uint8_t n, isr_handler_t handler) {
    interrupt_handlers[n] = handler;
}

static const char *exception_names[32] = {
    "Divide by zero", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 FP exception", "Alignment check",
    "Machine check", "SIMD FP exception", "Virtualization exception",
    "Control protection exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor injection",
    "VMM communication", "Security exception", "Reserved"
};

void isr_dispatch(registers_t *regs) {
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
        return;
    }
    kprintf("\n[PANIC] CPU exception %d: %s (err=%x)\n",
            (int)regs->int_no, exception_names[regs->int_no], regs->err_code);
    serial_write("[panic] unhandled CPU exception, halting\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

void irq_dispatch(registers_t *regs) {
    uint8_t irq = (uint8_t)(regs->int_no - 32);
    if (interrupt_handlers[regs->int_no]) {
        interrupt_handlers[regs->int_no](regs);
    }
    pic_send_eoi(irq);
}

void idt_init(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;
    memset(&idt, 0, sizeof(idt));

    pic_remap();

    #define SET(n) idt_set_gate(n, (uint64_t)isr##n, 0x08, 0x8E)
    SET(0);SET(1);SET(2);SET(3);SET(4);SET(5);SET(6);SET(7);
    SET(8);SET(9);SET(10);SET(11);SET(12);SET(13);SET(14);SET(15);
    SET(16);SET(17);SET(18);SET(19);SET(20);SET(21);SET(22);SET(23);
    SET(24);SET(25);SET(26);SET(27);SET(28);SET(29);SET(30);SET(31);
    #undef SET

    #define SETIRQ(n) idt_set_gate(32 + n, (uint64_t)irq##n, 0x08, 0x8E)
    SETIRQ(0);SETIRQ(1);SETIRQ(2);SETIRQ(3);SETIRQ(4);SETIRQ(5);SETIRQ(6);SETIRQ(7);
    SETIRQ(8);SETIRQ(9);SETIRQ(10);SETIRQ(11);SETIRQ(12);SETIRQ(13);SETIRQ(14);SETIRQ(15);
    #undef SETIRQ

    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
    __asm__ volatile ("sti");
    serial_write("[idt] loaded, PIC remapped, interrupts enabled\n");
}
