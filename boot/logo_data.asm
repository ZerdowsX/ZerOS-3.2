; logo_data.asm - embeds the boot splash logo as raw RGB888 pixel data
; (400x400x3 bytes, generated at build time from the user's JPG logo)

section .rodata
align 8
global logo_data
logo_data:
    incbin "boot/logo.bin"
global logo_data_end
logo_data_end:
