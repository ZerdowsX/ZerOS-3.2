; boot.asm - Nugget OS entry point
; Multiboot2 header + 32-bit protected mode entry
; Sets up paging (identity map first 1GB), enables long mode,
; then jumps into 64-bit code and calls the C kernel.

bits 32

section .multiboot2
align 8
mb2_header_start:
    dd 0xE85250D6                ; magic number
    dd 0                         ; architecture: i386
    dd mb2_header_end - mb2_header_start
    dd -(0xE85250D6 + 0 + (mb2_header_end - mb2_header_start))

    ; framebuffer request tag - ask GRUB to set up a linear framebuffer
    align 8
    dw 5                         ; type = framebuffer
    dw 1                         ; flags = optional (don't fail boot if unavailable)
    dd 20                        ; size
    dd 1024                      ; preferred width
    dd 768                       ; preferred height
    dd 32                        ; preferred bpp

    ; end tag
    align 8
    dw 0                         ; type
    dw 0                         ; flags
    dd 8                         ; size
mb2_header_end:

section .bss
align 16
stack_bottom:
    resb 65536                   ; 64 KiB stack
stack_top:

global pd0
global pd1
global pd2
global pd3

align 4096
pml4:       resb 4096
pdpt:       resb 4096
pd0:        resb 4096
pd1:        resb 4096
pd2:        resb 4096
pd3:        resb 4096

align 4
saved_mb2_ptr: resd 1

section .data
align 8
gdt64:
    dq 0                                    ; null descriptor
.code_seg: equ $ - gdt64
    dq 0x00AF9A000000FFFF                   ; 64-bit code segment
.data_seg: equ $ - gdt64
    dq 0x00AF92000000FFFF                   ; 64-bit data segment
gdt64_ptr:
    dw $ - gdt64 - 1
    dq gdt64

section .text
global _start
extern kernel_main

_start:
    cli
    mov esp, stack_top
    mov [saved_mb2_ptr], ebx      ; save multiboot2 info pointer somewhere the later rsp reset won't clobber it

    ; --- Build page tables: identity map first 4 GiB using 2MiB pages ---
    ; PML4[0] -> PDPT
    mov eax, pdpt
    or eax, 0b11                 ; present + writable
    mov [pml4], eax
    mov dword [pml4 + 4], 0

    ; PDPT[0..3] -> pd0..pd3 (each PD covers 1 GiB)
    mov eax, pd0
    or eax, 0b11
    mov [pdpt], eax
    mov dword [pdpt + 4], 0

    mov eax, pd1
    or eax, 0b11
    mov [pdpt + 8], eax
    mov dword [pdpt + 12], 0

    mov eax, pd2
    or eax, 0b11
    mov [pdpt + 16], eax
    mov dword [pdpt + 20], 0

    mov eax, pd3
    or eax, 0b11
    mov [pdpt + 24], eax
    mov dword [pdpt + 28], 0

    ; Fill each PD with 512 entries of 2MiB pages
    ; edx = which GiB (0-3), ecx = entry within that GiB (0-511)
    mov edx, 0
.fill_pd_outer:
    mov eax, pd0
    cmp edx, 0
    je .got_pd
    mov eax, pd1
    cmp edx, 1
    je .got_pd
    mov eax, pd2
    cmp edx, 2
    je .got_pd
    mov eax, pd3
.got_pd:
    mov edi, eax                 ; edi = base address of this PD table

    mov ecx, 0
.fill_pd_inner:
    ; physical address = edx * 1GiB + ecx * 2MiB
    mov eax, edx
    shl eax, 30                  ; edx * 1GiB (only valid while edx<4, fits in 32 bits up to 3<<30)
    push edx
    mov edx, ecx
    shl edx, 21                  ; ecx * 2MiB
    add eax, edx
    pop edx
    or eax, 0b10000011           ; present + writable + huge page (PS bit)
    mov [edi + ecx*8], eax
    mov dword [edi + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jne .fill_pd_inner

    inc edx
    cmp edx, 4
    jne .fill_pd_outer

    ; Load PML4 into cr3
    mov eax, pml4
    mov cr3, eax

    ; Enable PAE (CR4 bit 5)
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Enable long mode bit in EFER MSR
    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; Enable paging + protection (CR0 bit 31 and bit 0)
    mov eax, cr0
    or eax, (1 << 31) | 1
    mov cr0, eax

    ; Load 64-bit GDT
    lgdt [gdt64_ptr]

    ; Far jump into 64-bit code segment
    jmp 0x08:long_mode_start

bits 64
long_mode_start:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    mov rsp, stack_top
    mov edi, [saved_mb2_ptr]     ; multiboot2 info pointer -> first arg to kernel_main
    and rsp, -16                 ; align stack to 16 bytes

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
