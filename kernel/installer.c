#include "installer.h"
#include "fb.h"
#include "font.h"
#include "cursor.h"
#include "mouse.h"
#include "keyboard.h"
#include "nuggetfs.h"
#include "timer.h"
#include "string.h"
#include "serial.h"
#include "bmp.h"
#include "heap.h"

#define INST_MARKER_NAME "SYSTEM.CFG"

static uint32_t COL_BG, COL_PANEL, COL_LIGHT, COL_SHADOW, COL_DARKSHADOW;
static uint32_t COL_TEXT, COL_TITLE_BG, COL_TITLE_TEXT, COL_PROGRESS;

static void init_colors(void) {
    COL_BG          = fb_pack_color(0x00, 0x80, 0x80);
    COL_PANEL       = fb_pack_color(0xC0, 0xC0, 0xC0);
    COL_LIGHT       = fb_pack_color(0xFF, 0xFF, 0xFF);
    COL_SHADOW      = fb_pack_color(0x80, 0x80, 0x80);
    COL_DARKSHADOW  = fb_pack_color(0x00, 0x00, 0x00);
    COL_TEXT        = fb_pack_color(0x00, 0x00, 0x00);
    COL_TITLE_BG    = fb_pack_color(0x00, 0x00, 0x82);
    COL_TITLE_TEXT  = fb_pack_color(0xFF, 0xFF, 0xFF);
    COL_PROGRESS    = fb_pack_color(0x00, 0x00, 0x82);
}

static void draw_bevel(int32_t x, int32_t y, int32_t w, int32_t h, bool raised) {
    uint32_t outer = raised ? COL_LIGHT : COL_DARKSHADOW;
    uint32_t outer_opposite = raised ? COL_DARKSHADOW : COL_LIGHT;
    uint32_t inner = raised ? COL_PANEL : COL_SHADOW;
    uint32_t inner_opposite = raised ? COL_SHADOW : COL_PANEL;

    fb_fill_rect(x, y, w, 1, outer);
    fb_fill_rect(x, y, 1, h, outer);
    fb_fill_rect(x, y + h - 1, w, 1, outer_opposite);
    fb_fill_rect(x + w - 1, y, 1, h, outer_opposite);
    if (w > 2 && h > 2) {
        fb_fill_rect(x + 1, y + 1, w - 2, 1, inner);
        fb_fill_rect(x + 1, y + 1, 1, h - 2, inner);
        fb_fill_rect(x + 1, y + h - 2, w - 2, 1, inner_opposite);
        fb_fill_rect(x + w - 2, y + 1, 1, h - 2, inner_opposite);
    }
}

static bool point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static void draw_button(int32_t x, int32_t y, int32_t w, int32_t h, const char *label) {
    fb_fill_rect(x, y, w, h, COL_PANEL);
    draw_bevel(x, y, w, h, true);
    int32_t tw = font_string_width(label, 2);
    font_draw_string(x + (w - tw) / 2, y + (h - font_char_height(2)) / 2, label, COL_TEXT, 2);
}

/* Waits for a fresh left-click anywhere, returning its coordinates.
   Keeps the cursor animated and redraws nothing itself - caller owns that. */
static void wait_for_click(int32_t *out_x, int32_t *out_y) {
    bool prev_left = false;
    int32_t last_mx = -1, last_my = -1;
    for (;;) {
        mouse_state_t m = mouse_get_state();
        if (m.x != last_mx || m.y != last_my) {
            cursor_move_to(m.x, m.y);
            last_mx = m.x; last_my = m.y;
            fb_present();
        }
        if (m.left && !prev_left) {
            *out_x = m.x;
            *out_y = m.y;
            prev_left = m.left;
            return;
        }
        prev_left = m.left;
        __asm__ volatile ("sti; hlt");
    }
}

static void panel_rect(int32_t *px, int32_t *py, int32_t *pw, int32_t *ph) {
    int32_t sw = (int32_t)fb_width(), sh = (int32_t)fb_height();
    *pw = 520;
    *ph = 360;
    *px = (sw - *pw) / 2;
    *py = (sh - *ph) / 2;
}

static void draw_panel_frame(const char *title) {
    int32_t px, py, pw, ph;
    panel_rect(&px, &py, &pw, &ph);

    fb_clear(COL_BG);
    fb_fill_rect(px - 2, py - 2, pw + 4, ph + 4, COL_DARKSHADOW);
    draw_bevel(px - 1, py - 1, pw + 2, ph + 2, true);

    fb_fill_rect(px, py, pw, 26, COL_TITLE_BG);
    font_draw_string(px + 8, py + (26 - font_char_height(2)) / 2, title, COL_TITLE_TEXT, 2);

    fb_fill_rect(px, py + 26, pw, ph - 26, COL_PANEL);
}

/* Existing installations (formatted before .socks programs existed) never
   got software/notepad/notepad.socks written - installer_run() below only
   runs on a genuinely fresh disk. Called on every boot regardless, so an
   older install picks this up automatically instead of needing a full
   reinstall (which would lose the user's files). */
void software_ensure_builtin_programs(void) {
    if (!nfs_exists_path("software/notepad/notepad.socks")) {
        nfs_mkdir_path("software");
        nfs_mkdir_path("software/notepad");
        extern const uint8_t notepad_socks_data[];
        extern const uint8_t notepad_socks_data_end[];
        nfs_write_path("software/notepad/notepad.socks", notepad_socks_data,
                        (uint32_t)(notepad_socks_data_end - notepad_socks_data));
        extern const uint8_t notepad_icon_data[];
        extern const uint8_t notepad_icon_data_end[];
        nfs_write_path("software/notepad/icon.bmp", notepad_icon_data,
                        (uint32_t)(notepad_icon_data_end - notepad_icon_data));
        serial_write("[software] backfilled missing notepad.socks on existing install\n");
    }
    if (!nfs_exists_path("software/calculator/calculator.socks")) {
        nfs_mkdir_path("software");
        nfs_mkdir_path("software/calculator");
        extern const uint8_t calculator_socks_data[];
        extern const uint8_t calculator_socks_data_end[];
        nfs_write_path("software/calculator/calculator.socks", calculator_socks_data,
                        (uint32_t)(calculator_socks_data_end - calculator_socks_data));
        extern const uint8_t calculator_icon_data[];
        extern const uint8_t calculator_icon_data_end[];
        nfs_write_path("software/calculator/icon.bmp", calculator_icon_data,
                        (uint32_t)(calculator_icon_data_end - calculator_icon_data));
        serial_write("[software] backfilled missing calculator.socks on existing install\n");
    }
}

bool installer_needed(void) {
    if (!nfs_mount()) return true;
    char marker[8];
    int n = nfs_read_path(INST_MARKER_NAME, marker, sizeof(marker));
    if (n > 0) return false;

    /* Marker missing, but this doesn't necessarily mean the disk is blank -
       someone could have deleted it by mistake. If real user folders are
       still there, this disk has clearly already been set up: silently
       recreate the marker instead of running the wizard again, which would
       reformat and wipe everything. Only show the wizard (and format) when
       the disk genuinely looks untouched. */
    if (nfs_exists_path("Documents") || nfs_exists_path("Desktop") || nfs_exists_path("Downloads")) {
        nfs_write_path(INST_MARKER_NAME, "1", 1);
        return false;
    }
    return true;
}

static void screen_welcome(void) {
    int32_t px, py, pw, ph;
    panel_rect(&px, &py, &pw, &ph);
    draw_panel_frame("WELCOME TO NUGGET");

    int32_t ty = py + 26 + 20;
    font_draw_string(px + 20, ty, "THIS WIZARD WILL SET UP NUGGET OS", COL_TEXT, 2);
    ty += 40;
    font_draw_string(px + 20, ty, "ON YOUR DISK.", COL_TEXT, 2);
    ty += 50;
    font_draw_string(px + 20, ty, "CLICK NEXT TO CHOOSE A DISK AND", COL_TEXT, 1);
    ty += 20;
    font_draw_string(px + 20, ty, "BEGIN INSTALLATION.", COL_TEXT, 1);

    int32_t bw = 100, bh = 32;
    int32_t bx = px + pw - bw - 20;
    int32_t by = py + ph - bh - 16;
    draw_button(bx, by, bw, bh, "NEXT");

    cursor_reset(mouse_get_state().x, mouse_get_state().y);
    fb_present();

    for (;;) {
        int32_t cx, cy;
        wait_for_click(&cx, &cy);
        if (point_in_rect(cx, cy, bx, by, bw, bh)) return;
    }
}

static bool screen_disk(void) {
    int32_t px, py, pw, ph;
    panel_rect(&px, &py, &pw, &ph);
    draw_panel_frame("SELECT DISK");

    int32_t ty = py + 26 + 20;
    font_draw_string(px + 20, ty, "THE FOLLOWING DISK WAS FOUND:", COL_TEXT, 1);
    ty += 30;

    int32_t diskbox_h = 40;
    fb_fill_rect(px + 20, ty, pw - 40, diskbox_h, COL_LIGHT);
    draw_bevel(px + 20, ty, pw - 40, diskbox_h, false);
    font_draw_string(px + 30, ty + (diskbox_h - font_char_height(1)) / 2,
                      "DISK 0 - ATA/PIO - 32 MB - UNFORMATTED", COL_TEXT, 1);
    ty += diskbox_h + 30;

    font_draw_string(px + 20, ty, "CLICKING FORMAT WILL ERASE ALL DATA", COL_TEXT, 1);
    ty += 18;
    font_draw_string(px + 20, ty, "ON THIS DISK AND INSTALL NUGGETFS.", COL_TEXT, 1);

    int32_t bw = 100, bh = 32;
    int32_t bx = px + pw - bw - 20;
    int32_t by = py + ph - bh - 16;
    draw_button(bx, by, bw, bh, "FORMAT");

    int32_t backw = 100;
    draw_button(px + 20, by, backw, bh, "CANCEL");

    cursor_reset(mouse_get_state().x, mouse_get_state().y);
    fb_present();

    for (;;) {
        int32_t cx, cy;
        wait_for_click(&cx, &cy);
        if (point_in_rect(cx, cy, bx, by, bw, bh)) return true;
        if (point_in_rect(cx, cy, px + 20, by, backw, bh)) return false;
    }
}

static void screen_installing(void) {
    int32_t px, py, pw, ph;
    panel_rect(&px, &py, &pw, &ph);
    draw_panel_frame("INSTALLING");

    int32_t ty = py + 26 + 30;
    font_draw_string(px + 20, ty, "FORMATTING DISK AND COPYING FILES...", COL_TEXT, 1);
    ty += 40;

    int32_t barw = pw - 40, barh = 24;
    int32_t barx = px + 20, bary = ty;
    fb_fill_rect(barx, bary, barw, barh, COL_LIGHT);
    draw_bevel(barx, bary, barw, barh, false);

    cursor_reset(mouse_get_state().x, mouse_get_state().y);
    fb_present();

    /* Real work: format the disk. We animate the bar across a few steps
       around the actual format call so it doesn't look instantaneous. */
    for (int step = 0; step <= 4; step++) {
        int32_t fillw = (barw - 4) * step / 4;
        fb_fill_rect(barx + 2, bary + 2, fillw, barh - 4, COL_PROGRESS);
        fb_present();
        if (step == 2) {
            nfs_format(65536); /* the 32 MiB disk image size assumed elsewhere too */
        }
        uint64_t start = timer_ticks();
        while (timer_ticks() - start < 30) { __asm__ volatile ("sti; hlt"); }
    }

    nfs_mount();
    nfs_write_path(INST_MARKER_NAME, "1", 1);
    nfs_write_path("welcome.txt", "Welcome to Nugget OS!\n", 22);
    nfs_mkdir_path("Documents");
    nfs_mkdir_path("Downloads");
    nfs_mkdir_path("Desktop");
    nfs_mkdir_path("Recycle Bin");
    nfs_mkdir_path("Wallpapers");
    nfs_write_path("Desktop/ZerBrowser.lnk", "4", 1);
    software_ensure_builtin_programs();
    /* Not auto-generating a Dandelion.bmp backup here anymore - writing a
       ~2.3MB file turned out to take uncomfortably long with our simple
       polling ATA driver, and doing that unconditionally during first-time
       setup risked making the installer itself hang for new users. Users
       can still make their own smaller wallpaper images in Paint. */
}

static void screen_done(void) {
    int32_t px, py, pw, ph;
    panel_rect(&px, &py, &pw, &ph);
    draw_panel_frame("SETUP COMPLETE");

    int32_t ty = py + 26 + 30;
    font_draw_string(px + 20, ty, "NUGGET OS HAS BEEN INSTALLED.", COL_TEXT, 2);
    ty += 40;
    font_draw_string(px + 20, ty, "STARTING THE DESKTOP NOW...", COL_TEXT, 1);
    fb_present();

    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 150) { __asm__ volatile ("sti; hlt"); } /* ~1.5s */
}

void installer_run(void) {
    if (!fb_available()) return;
    init_colors();
    cursor_init();
    serial_write("[installer] running setup wizard\n");

    for (;;) {
        screen_welcome();
        if (screen_disk()) break;
        /* CANCEL just loops back to the welcome screen */
    }
    screen_installing();
    screen_done();
    serial_write("[installer] setup complete\n");
}
