; wallpaper_data.asm - embeds the desktop wallpaper as raw RGB888 pixel data
; (1024x768x3 bytes, generated at build time from the user's JPG wallpaper)

section .rodata
align 8
global wallpaper_data
wallpaper_data:
    incbin "boot/wallpaper.bin"
global wallpaper_data_end
wallpaper_data_end:
