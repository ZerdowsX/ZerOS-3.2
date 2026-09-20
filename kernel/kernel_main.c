#include "types.h"
#include "vga.h"
#include "serial.h"
#include "gdt.h"
#include "idt.h"
#include "pmm.h"
#include "heap.h"
#include "timer.h"
#include "fb.h"
#include "splash.h"
#include "font.h"
#include "cursor.h"
#include "mouse.h"
#include "keyboard.h"
#include "ata.h"
#include "nuggetfs.h"
#include "installer.h"
#include "wallpaper.h"
#include "socks.h"
#include "wm.h"
#include "net.h"
#include "dhcp.h"
#include "rtl8139.h"
#include "pcnet.h"
#include "e1000.h"

/* Defined by linker.ld - marks the first free byte right after the kernel
   image (code + rodata + data + bss). Only its address matters, so it's
   declared as an opaque symbol rather than given a real type. */
extern uint8_t kernel_end;

/* Static bump-allocator heap region. Placed well above the low-memory
   area used by the .socks loader (SOCKS_API_ADDR/SOCKS_LOAD_ADDR sit
   around 0x6FF000-0x700000) and above the kernel image itself, which
   embeds the boot logo/wallpaper/icon bitmaps and can run to a few MiB. */
#define HEAP_START 0x1000000ULL   /* 16 MiB */
#define HEAP_SIZE  0x4000000ULL   /* 64 MiB */

/* Timer frequency - the network stack's blocking waits (net_resolve_arp_blocking,
   dhcp_negotiate) assume 100 ticks/sec. */
#define TIMER_HZ 100

/* Fallback static network config, used only if DHCP doesn't get an answer. */
#define FALLBACK_IP      MAKE_IP(10, 0, 2, 15)
#define FALLBACK_NETMASK MAKE_IP(255, 255, 255, 0)
#define FALLBACK_GATEWAY MAKE_IP(10, 0, 2, 2)

/* Brings up whichever NIC actually exists (RTL8139 first - what QEMU
   emulates by default - then PCNet for VirtualBox, then e1000), wires
   its send/get_mac/rx-callback into the NIC-agnostic net.c layer, and
   returns true if any driver was found. */
static bool net_bring_up_nic(void) {
    if (rtl8139_init()) {
        rtl8139_set_rx_callback(net_rx_handler_entry);
        net_set_nic(rtl8139_send, rtl8139_get_mac);
        serial_write("[net] using RTL8139\n");
        return true;
    }
    if (pcnet_init()) {
        pcnet_set_rx_callback(net_rx_handler_entry);
        net_set_nic(pcnet_send, pcnet_get_mac);
        serial_write("[net] using PCNet\n");
        return true;
    }
    if (e1000_init()) {
        e1000_set_rx_callback(net_rx_handler_entry);
        net_set_nic(e1000_send, e1000_get_mac);
        serial_write("[net] using e1000\n");
        return true;
    }
    serial_write("[net] no supported NIC found\n");
    return false;
}

void kernel_main(uint32_t mb2_info_addr) {
    /* --- Early debug output, before we can trust anything graphical --- */
    serial_init();
    serial_write("\n[boot] Nugget OS starting\n");

    /* --- CPU/memory plumbing --- */
    gdt_init();
    idt_init();
    pmm_init(mb2_info_addr, (uint64_t)&kernel_end);
    heap_init(HEAP_START, HEAP_SIZE);
    timer_init(TIMER_HZ);
    serial_write("[boot] gdt/idt/pmm/heap/timer ready\n");

    /* --- Graphics --- */
    bool have_fb = fb_init(mb2_info_addr);
    if (have_fb) {
        splash_show();
        splash_set_progress(10);
    } else {
        serial_write("[boot] WARNING: no framebuffer, falling back to text output\n");
        vga_clear();
        vga_write("Nugget OS: no framebuffer available.\n");
    }

    /* --- Storage / filesystem --- */
    ata_init();
    if (have_fb) splash_set_progress(35);
    if (installer_needed()) {
        installer_run();
    }
    software_ensure_builtin_programs();
    wallpaper_restore_saved_choice();
    socks_api_init();
    if (have_fb) splash_set_progress(55);
    serial_write("[boot] storage/filesystem ready\n");

    /* --- Input --- */
    keyboard_init();
    mouse_init();
    if (have_fb) {
        mouse_set_bounds((int32_t)fb_width(), (int32_t)fb_height());
        cursor_init();
    }

    /* --- Networking (best-effort - a missing/unsupported NIC isn't fatal) --- */
    net_init();
    if (net_bring_up_nic()) {
        uint32_t ip, netmask, gateway;
        if (dhcp_negotiate(&ip, &netmask, &gateway)) {
            net_set_ip_config(ip, netmask, gateway);
            serial_write("[net] DHCP lease acquired\n");
        } else {
            net_set_ip_config(FALLBACK_IP, FALLBACK_NETMASK, FALLBACK_GATEWAY);
            serial_write("[net] DHCP failed, using fallback static IP\n");
        }
    }
    if (have_fb) splash_set_progress(90);

    /* --- Desktop --- */
    if (have_fb) {
        splash_set_progress(100);
        splash_clear();
        wm_init();
    }
    serial_write("[boot] desktop ready\n");

    __asm__ volatile ("sti");

    if (!have_fb) {
        /* No framebuffer, nothing sensible to render - just idle. */
        for (;;) __asm__ volatile ("hlt");
    }

    /* --- Main loop --- */
    for (;;) {
        mouse_state_t ms = mouse_get_state();
        wm_update(ms.x, ms.y, ms.left, ms.right);

        while (keyboard_has_data()) {
            char c = keyboard_getchar();
            if (c) wm_handle_key(c);
        }

        __asm__ volatile ("hlt");
    }
}
