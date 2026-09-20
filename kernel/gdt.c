#include "gdt.h"
#include "serial.h"

/*
 * The flat 64-bit GDT (null, code, data descriptors) is already loaded
 * by boot.asm before entering long mode - that's sufficient for a
 * ring-0-only kernel. This module is a placeholder for when we add
 * user-mode (ring 3) processes and a TSS for privilege-level switches.
 */
void gdt_init(void) {
    serial_write("[gdt] using flat GDT from boot.asm (ring 0 only)\n");
}
