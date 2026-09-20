CC = gcc
LD = ld
ASM = nasm

CFLAGS = -m64 -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -Wall -Wextra -Iinclude -c
LDFLAGS = -n -T linker.ld -nostdlib
ASMFLAGS = -f elf64

BUILD_DIR = build
KERNEL = $(BUILD_DIR)/nugget.elf

C_SOURCES := $(shell find kernel drivers fs net lib -name '*.c')
ASM_SOURCES := boot/boot.asm boot/logo_data.asm boot/wallpaper_data.asm boot/icon_data.asm boot/software_data.asm kernel/int/isr.asm

C_OBJECTS := $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
ASM_OBJECTS := $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))

.PHONY: all clean iso run kernel

all: iso

kernel: $(KERNEL)

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.asm
	@mkdir -p $(dir $@)
	$(ASM) $(ASMFLAGS) $< -o $@

$(KERNEL): $(ASM_OBJECTS) $(C_OBJECTS) linker.ld
	$(LD) $(LDFLAGS) -o $(KERNEL) $(ASM_OBJECTS) $(C_OBJECTS)

iso: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/nugget.elf
	cp grub.cfg iso/boot/grub/grub.cfg
	grub-mkrescue -o nugget-os.iso iso

run: iso
	qemu-system-x86_64 -cdrom nugget-os.iso -m 256M \
		-drive file=disk.img,format=raw,if=ide \
		-netdev user,id=net0 -device rtl8139,netdev=net0 \
		-serial stdio

clean:
	rm -rf $(BUILD_DIR) nugget-os.iso
