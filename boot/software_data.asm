; software_data.asm - embeds built .socks program files so the installer
; can write them onto the NuggetFS disk under software/<name>/ at
; install time, giving a fresh install its starting set of programs.

section .rodata
align 8
global notepad_socks_data
notepad_socks_data:
    incbin "boot/software/notepad.socks"
global notepad_socks_data_end
notepad_socks_data_end:

global notepad_icon_data
notepad_icon_data:
    incbin "boot/software/notepad_icon.bmp"
global notepad_icon_data_end
notepad_icon_data_end:

global calculator_socks_data
calculator_socks_data:
    incbin "boot/software/calculator.socks"
global calculator_socks_data_end
calculator_socks_data_end:

global calculator_icon_data
calculator_icon_data:
    incbin "boot/software/calculator_icon.bmp"
global calculator_icon_data_end
calculator_icon_data_end:
