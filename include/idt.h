#ifndef NUGGET_IDT_H
#define NUGGET_IDT_H
#include "types.h"

typedef struct PACKED {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers_t;

typedef void (*isr_handler_t)(registers_t *regs);

void idt_init(void);
void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector, uint8_t flags);
void register_interrupt_handler(uint8_t n, isr_handler_t handler);
void irq_clear_mask(uint8_t irq_line);
void irq_set_mask(uint8_t irq_line);

#endif
