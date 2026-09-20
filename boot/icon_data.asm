; icon_data.asm - embeds the real (PNG-sourced) icon set as raw RGBA pixel data
; each icon is stored at 32x32 and 16x16, both with alpha channel

section .rodata
align 8
global icon_folder_32
icon_folder_32:
    incbin "boot/icons/folder_32.bin"

global icon_folder_16
icon_folder_16:
    incbin "boot/icons/folder_16.bin"

global icon_disk_32
icon_disk_32:
    incbin "boot/icons/disk_32.bin"

global icon_disk_16
icon_disk_16:
    incbin "boot/icons/disk_16.bin"

global icon_notepad_32
icon_notepad_32:
    incbin "boot/icons/notepad_32.bin"

global icon_notepad_16
icon_notepad_16:
    incbin "boot/icons/notepad_16.bin"

global icon_file_32
icon_file_32:
    incbin "boot/icons/file_32.bin"

global icon_file_16
icon_file_16:
    incbin "boot/icons/file_16.bin"

global icon_paint_32
icon_paint_32:
    incbin "boot/icons/paint_32.bin"

global icon_paint_16
icon_paint_16:
    incbin "boot/icons/paint_16.bin"

global icon_terminal_32
icon_terminal_32:
    incbin "boot/icons/terminal_32.bin"

global icon_terminal_16
icon_terminal_16:
    incbin "boot/icons/terminal_16.bin"

global icon_browser_32
icon_browser_32:
    incbin "boot/icons/browser_32.bin"

global icon_browser_16
icon_browser_16:
    incbin "boot/icons/browser_16.bin"

global icon_calculator_32
icon_calculator_32:
    incbin "boot/icons/calculator_32.bin"

global icon_calculator_16
icon_calculator_16:
    incbin "boot/icons/calculator_16.bin"

global icon_trash_full_32
icon_trash_full_32:
    incbin "boot/icons/trash_full_32.bin"

global icon_trash_full_16
icon_trash_full_16:
    incbin "boot/icons/trash_full_16.bin"

global icon_trash_empty_32
icon_trash_empty_32:
    incbin "boot/icons/trash_empty_32.bin"

global icon_trash_empty_16
icon_trash_empty_16:
    incbin "boot/icons/trash_empty_16.bin"
