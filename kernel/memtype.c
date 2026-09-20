#include "types.h"
#include "serial.h"

/* The four 1GiB page directories built in boot.asm, each holding 512
   2MiB-page entries (PDEs). Exposed from assembly so we can locate and
   adjust the specific entries covering the framebuffer. */
extern uint64_t pd0[512];
extern uint64_t pd1[512];
extern uint64_t pd2[512];
extern uint64_t pd3[512];

#define PDE_PWT (1ULL << 3)   /* selects PAT slot bit 0 (with PCD=0, PAT-bit=0) */
#define PAT_MSR 0x277

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}

static uint64_t *pd_for_gib(int gib) {
    switch (gib) {
        case 0: return pd0;
        case 1: return pd1;
        case 2: return pd2;
        case 3: return pd3;
        default: return NULL;
    }
}

/* Marks the 2MiB huge page(s) covering [phys_addr, phys_addr+len) as
   write-combining, and makes sure the PAT MSR actually has a
   write-combining entry to select. Only touches the specific pages in
   that range - nothing else about memory typing changes. Returns false
   (and touches nothing) if the range doesn't fit our simple 4GiB
   identity-mapped, 2MiB-huge-page setup, so callers can safely fall back
   to normal (uncached-by-default) behavior instead. */
bool memtype_set_write_combining(uint64_t phys_addr, uint64_t len) {
    uint64_t start = phys_addr & ~((uint64_t)0x1FFFFF); /* round down to 2MiB */
    uint64_t end = (phys_addr + len + 0x1FFFFF) & ~((uint64_t)0x1FFFFF); /* round up */

    if (end > (4ULL * 1024 * 1024 * 1024)) {
        serial_write("[memtype] framebuffer range exceeds our 4GiB identity map, skipping WC setup\n");
        return false;
    }

    /* PAT entry 1 defaults to Write-Through, which nothing here uses -
       repurpose it as Write-Combining (type 1). Leave every other entry
       untouched, so ordinary WB/UC memory elsewhere is unaffected. */
    uint64_t pat = rdmsr(PAT_MSR);
    uint64_t new_pat = (pat & ~((uint64_t)0xFF << 8)) | ((uint64_t)0x01 << 8);
    wrmsr(PAT_MSR, new_pat);

    for (uint64_t addr = start; addr < end; addr += 0x200000) {
        int gib = (int)(addr >> 30);
        uint32_t idx = (uint32_t)((addr >> 21) & 0x1FF);
        uint64_t *pd = pd_for_gib(gib);
        if (!pd) {
            serial_write("[memtype] address out of expected range, aborting WC setup\n");
            return false;
        }
        pd[idx] |= PDE_PWT;
    }

    /* Flush the TLB so the new page attributes actually take effect. */
    __asm__ volatile (
        "mov %%cr3, %%rax\n\t"
        "mov %%rax, %%cr3\n\t"
        ::: "rax"
    );

    serial_write("[memtype] framebuffer marked write-combining\n");
    return true;
}
