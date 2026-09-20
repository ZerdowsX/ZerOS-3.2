#include "wm.h"
#include "fb.h"
#include "font.h"
#include "rtc.h"
#include "cursor.h"
#include "string.h"
#include "serial.h"
#include "icons.h"
#include "heap.h"
#include "rtl8139.h"
#include "pcnet.h"
#include "nuggetfs.h"
#include "types.h"
#include "wallpaper.h"
#include "bmp.h"
#include "tcp.h"
#include "net.h"
#include "dns.h"
#include "socks.h"

#define EXPLORER_MAX_FILES 16

typedef enum { PAINT_TOOL_BRUSH, PAINT_TOOL_ERASER, PAINT_TOOL_BUCKET,
               PAINT_TOOL_LINE, PAINT_TOOL_RECT, PAINT_TOOL_ELLIPSE, PAINT_TOOL_TEXT } paint_tool_t;

typedef struct {
    bool in_use;
    bool minimized;
    bool maximized;
    int32_t restore_x, restore_y, restore_w, restore_h;
    int32_t x, y, w, h;
    char title[WM_TITLE_MAX];
    uint32_t content_color;
    app_type_t app_type;

    /* Notepad */
    char text[512];
    int32_t text_len;
    char notepad_path[NFS_MAX_PATH]; /* "" = unsaved new file */
    char socks_path[NFS_MAX_PATH];   /* which .socks program this window runs (APP_SOCKS) */
    socks_ctx_t socks_ctx;           /* per-window state for a .socks-based program */
    bool socks_ready;                /* has init() been called for this window yet */
    bool notepad_saving_as;
    char notepad_saveas_buf[NFS_MAX_PATH];
    int32_t notepad_saveas_len;

    /* Calculator (fixed-point, scaled by CALC_SCALE - no floats/doubles: SSE is disabled) */
    char calc_display[24];
    int64_t calc_accum;
    char calc_pending_op;
    bool calc_fresh_entry;

    /* Paint */
    uint32_t *paint_canvas;
    int32_t paint_canvas_w, paint_canvas_h;
    paint_tool_t paint_tool;
    uint32_t paint_color;
    int32_t paint_last_x, paint_last_y; /* canvas-local; -1 = no previous point this stroke */
    int32_t paint_shape_start_x, paint_shape_start_y;
    bool paint_shape_dragging;
    uint32_t *paint_undo_canvas;
    int32_t paint_undo_w, paint_undo_h;
    bool paint_text_active;
    int32_t paint_text_x, paint_text_y;
    char paint_path[NFS_MAX_PATH];
    bool paint_saving_as;
    char paint_saveas_buf[NFS_MAX_PATH];
    int32_t paint_saveas_len;

    /* Explorer */
    char explorer_path[NFS_MAX_PATH]; /* "" or "/" = root */

    /* Cached directory listing - draw_explorer used to call nfs_list_path
       (real disk reads) on every single redraw, which made dragging an
       Explorer window dramatically slower than other apps since it was
       hitting the disk dozens of times a second. Now it only re-reads
       when the path changes or something explicitly invalidates it. */
    bool explorer_cache_valid;
    char explorer_cache_path[NFS_MAX_PATH];
    char explorer_cache_names[EXPLORER_MAX_FILES][32];
    uint32_t explorer_cache_sizes[EXPLORER_MAX_FILES];
    bool explorer_cache_is_dir[EXPLORER_MAX_FILES];
    int32_t explorer_cache_count;

    /* Terminal */
    char term_output[2048];
    int32_t term_output_len;
    char term_cwd[NFS_MAX_PATH]; /* "" = root */
    char term_input[64];
    int32_t term_input_len;

    /* Browser (ZerBrowser) */
    char browser_url[64];
    int32_t browser_url_len;
    bool browser_editing_url;
    int browser_state; /* 0=idle 1=connecting 2=loading 3=done 4=error */
    void *browser_conn; /* tcp_connection_t*, opaque here to avoid a tcp.h dependency in the struct */
    char browser_response[3072];
    int32_t browser_response_len;
    uint32_t browser_target_ip;
    uint16_t browser_target_port;
    char browser_path[64];
    char browser_history[8][64];
    int browser_history_count;
    char browser_forward[8][64];
    int browser_forward_count;

    /* Parsed HTML, rebuilt once each time a page finishes loading */
    char browser_line_text[40][100];
    char browser_line_link[40][64];
    bool browser_line_heading[40];
    int32_t browser_line_count;
} window_t;

#define TITLEBAR_HEIGHT 22
#define BTN_SIZE        16
#define TASKBAR_HEIGHT  30
#define START_BTN_WIDTH 64
#define TASKBAR_BTN_WIDTH 140
#define TASKBAR_BTN_GAP 4
#define MENU_ITEM_HEIGHT 24
#define MENU_WIDTH 170
#define FONT_SCALE 1
#define PAINT_TOOLBAR_H 50
#define PAINT_PALETTE_COUNT 8
#define PAINT_SWATCH_SIZE 18

typedef struct {
    const char *label;
    icon_id_t icon;
    bool has_icon;
    app_type_t app_type;
    int power_action; /* 0 = open app, 1 = shutdown, 2 = restart */
} menu_item_t;

static const menu_item_t menu_items[] = {
    { "PAINT",      ICON_PAINT,      true, APP_PAINT,      0 },
    { "EXPLORER",   ICON_DISK,       true, APP_EXPLORER,   0 },
    { "ZERBROWSER", ICON_BROWSER,    true, APP_BROWSER,    0 },
    { "TERMINAL",   ICON_TERMINAL,   true, APP_TERMINAL,   0 },
    { "SHUTDOWN",   ICON_FILE,       false, APP_EMPTY,     1 },
    { "RESTART",    ICON_FILE,       false, APP_EMPTY,     2 },
};
#define MENU_ITEM_COUNT (int)(sizeof(menu_items) / sizeof(menu_items[0]))

/* Programs under software/<name>/<name>.socks are discovered fresh every
   time the Start menu is opened (not every frame it stays open - that
   would be the same "reads the disk far more often than needed" mistake
   we already found and fixed elsewhere). Delete a folder from software/
   and its program stops showing up here; drop a new folder with a
   .socks file in it and it appears - no kernel rebuild needed. */
static void path_join(const char *dir_path, const char *name, char *out, size_t out_size);
#define MAX_DISCOVERED_SOCKS 16
typedef struct {
    char name[32];
    char path[NFS_MAX_PATH];
    uint32_t icon_pixels[32 * 32]; /* decoded from software/<name>/icon.bmp if present */
    bool has_icon;
} discovered_program_t;
static discovered_program_t discovered_programs[MAX_DISCOVERED_SOCKS];
static int discovered_count = 0;
static char scan_parent_path[NFS_MAX_PATH];
static char scan_parent_name[NFS_MAX_NAME];

static void software_subitem_collect(const char *name, uint32_t size, bool is_dir) {
    (void)size;
    if (is_dir) return;
    size_t len = strlen(name);
    if (len > 6 && strcmp(name + len - 6, ".socks") == 0 && discovered_count < MAX_DISCOVERED_SOCKS) {
        int idx = discovered_count++;
        strncpy(discovered_programs[idx].name, scan_parent_name, sizeof(discovered_programs[idx].name) - 1);
        discovered_programs[idx].name[sizeof(discovered_programs[idx].name) - 1] = '\0';
        path_join(scan_parent_path, name, discovered_programs[idx].path, sizeof(discovered_programs[idx].path));

        char icon_path[NFS_MAX_PATH];
        path_join(scan_parent_path, "icon.bmp", icon_path, sizeof(icon_path));
        static uint8_t icon_scratch[8192];
        int n = nfs_read_path(icon_path, icon_scratch, sizeof(icon_scratch));
        int32_t iw, ih;
        discovered_programs[idx].has_icon =
            (n > 0) && bmp_decode(icon_scratch, n, discovered_programs[idx].icon_pixels, 32, 32, &iw, &ih);
    }
}
static void software_folder_collect(const char *name, uint32_t size, bool is_dir) {
    (void)size;
    if (!is_dir) return;
    strncpy(scan_parent_name, name, sizeof(scan_parent_name) - 1);
    scan_parent_name[sizeof(scan_parent_name) - 1] = '\0';
    path_join("software", name, scan_parent_path, sizeof(scan_parent_path));
    nfs_list_path(scan_parent_path, software_subitem_collect);
}
static void scan_software_folder(void) {
    discovered_count = 0;
    nfs_list_path("software", software_folder_collect);
}

static window_t windows[WM_MAX_WINDOWS];
static int32_t screen_w, screen_h;

static int z_order[WM_MAX_WINDOWS];
static int z_count = 0;

static int dragging_window = -1;
static int32_t drag_offset_x = 0, drag_offset_y = 0;
static int resizing_window = -1;
static int32_t resize_start_mouse_x = 0, resize_start_mouse_y = 0;
static int32_t resize_start_w = 0, resize_start_h = 0;
static int painting_window = -1;
static bool start_menu_open = false;
static bool prev_mouse_left = false;

/* Tracks the vertical range of the screen actually touched by the current
   frame's changes, so a window drag only needs to re-copy that band to
   real video memory (which turned out to be a genuinely slow ~30ms for
   the whole screen) instead of everything, most of which didn't change. */
static int32_t dirty_rect_y0 = -1, dirty_rect_y1 = -1;
static void expand_dirty_rows(int32_t y0, int32_t y1) {
    if (dirty_rect_y0 < 0 || y0 < dirty_rect_y0) dirty_rect_y0 = y0;
    if (dirty_rect_y1 < 0 || y1 > dirty_rect_y1) dirty_rect_y1 = y1;
}

/* Redraw throttling: continuous operations (dragging/resizing a window,
   an active Paint stroke) can generate mouse-move events far faster than
   any display can show - doing a full wallpaper+windows redraw and a
   whole-framebuffer copy on every single one wastes CPU the drag itself
   needs to stay responsive. One-off events (a click, opening a window)
   still redraw immediately since there's no flood of those to coalesce. */
static uint64_t last_redraw_tick = 0;
#define REDRAW_MIN_INTERVAL_TICKS 1 /* ~10ms at our 100 Hz timer, i.e. capped at ~100 redraws/sec */

/* Lightweight on-screen diagnostic: how many full redraws actually happen
   per second, so lag reports can be backed by a real number instead of
   "feels slow" - shown in the taskbar next to the clock. */
static int redraws_this_window = 0;
static uint64_t rps_window_start = 0;
static int last_rps = 0;
static bool prev_mouse_right = false;
static int32_t last_mouse_x = -1, last_mouse_y = -1;
static bool force_redraw = false;

static uint8_t last_hour = 255, last_minute = 255;
static char clock_text[6] = "--:--";

/* Explorer file listing scratch space (used for click hit-testing / context menus) */
static char explorer_names[EXPLORER_MAX_FILES][32];
static uint32_t explorer_sizes[EXPLORER_MAX_FILES];
static int explorer_count = 0;

/* Populates a window's cached directory listing - see the cache fields on
   window_t for why this exists (avoiding a disk read on every redraw). */
static window_t *explorer_cache_target = NULL;
static void explorer_cache_collect(const char *name, uint32_t size, bool is_dir) {
    if (!explorer_cache_target) return;
    int32_t *count = &explorer_cache_target->explorer_cache_count;
    if (*count < EXPLORER_MAX_FILES) {
        strncpy(explorer_cache_target->explorer_cache_names[*count], name, 31);
        explorer_cache_target->explorer_cache_names[*count][31] = '\0';
        explorer_cache_target->explorer_cache_sizes[*count] = size;
        explorer_cache_target->explorer_cache_is_dir[*count] = is_dir;
        (*count)++;
    }
}

static void explorer_refresh_cache_if_needed(window_t *w) {
    if (w->explorer_cache_valid && strcmp(w->explorer_cache_path, w->explorer_path) == 0) return;
    w->explorer_cache_count = 0;
    explorer_cache_target = w;
    nfs_list_path(w->explorer_path, explorer_cache_collect);
    explorer_cache_target = NULL;
    strncpy(w->explorer_cache_path, w->explorer_path, sizeof(w->explorer_cache_path) - 1);
    w->explorer_cache_path[sizeof(w->explorer_cache_path) - 1] = '\0';
    w->explorer_cache_valid = true;
}

/* Called after anything that could change a folder's contents (new file,
   delete, rename, paste, restore...) - simplest safe option is to just
   invalidate every open Explorer window's cache rather than track which
   ones might be showing the affected folder. */
static bool desktop_cache_valid = false;
/* Off by default - it was a diagnostic tool for chasing down the dragging
   lag, not something worth cluttering the taskbar with now that it's
   fixed. Toggle via the terminal's "fps on"/"fps off" command. */
static bool show_rps_counter = false;
static bool recycle_bin_empty_cache_valid = false;

static void invalidate_all_explorer_caches(void) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].app_type == APP_EXPLORER) {
            windows[i].explorer_cache_valid = false;
        }
    }
    desktop_cache_valid = false;
    recycle_bin_empty_cache_valid = false;
}

/* Classic Windows 9x-style palette */
static uint32_t COL_DESKTOP, COL_FACE, COL_FACE_LIGHT, COL_FACE_SHADOW, COL_FACE_DARKSHADOW;
static uint32_t COL_TITLEBAR_ACTIVE, COL_TITLEBAR_TEXT, COL_TEXT_BLACK, COL_WINDOW_CONTENT_DEFAULT;
static uint32_t COL_WHITE, COL_GREEN, COL_RED;
static uint32_t PAINT_PALETTE[PAINT_PALETTE_COUNT];

static void init_colors(void) {
    COL_DESKTOP              = fb_pack_color(0x00, 0x80, 0x80);
    COL_FACE                 = fb_pack_color(0xC0, 0xC0, 0xC0);
    COL_FACE_LIGHT           = fb_pack_color(0xFF, 0xFF, 0xFF);
    COL_FACE_SHADOW          = fb_pack_color(0x80, 0x80, 0x80);
    COL_FACE_DARKSHADOW      = fb_pack_color(0x00, 0x00, 0x00);
    COL_TITLEBAR_ACTIVE      = fb_pack_color(0x00, 0x00, 0x82);
    COL_TITLEBAR_TEXT        = fb_pack_color(0xFF, 0xFF, 0xFF);
    COL_TEXT_BLACK           = fb_pack_color(0x00, 0x00, 0x00);
    COL_WINDOW_CONTENT_DEFAULT = fb_pack_color(0xC0, 0xC0, 0xC0);
    COL_WHITE                = fb_pack_color(0xFF, 0xFF, 0xFF);
    COL_GREEN                = fb_pack_color(0x20, 0xA0, 0x20);
    COL_RED                  = fb_pack_color(0xC0, 0x30, 0x30);

    PAINT_PALETTE[0] = fb_pack_color(0x00, 0x00, 0x00);   /* black */
    PAINT_PALETTE[1] = fb_pack_color(0xFF, 0xFF, 0xFF);   /* white */
    PAINT_PALETTE[2] = fb_pack_color(0xE0, 0x30, 0x30);   /* red */
    PAINT_PALETTE[3] = fb_pack_color(0x30, 0xA0, 0x30);   /* green */
    PAINT_PALETTE[4] = fb_pack_color(0x30, 0x60, 0xE0);   /* blue */
    PAINT_PALETTE[5] = fb_pack_color(0xE0, 0xD0, 0x20);   /* yellow */
    PAINT_PALETTE[6] = fb_pack_color(0xE0, 0x90, 0x20);   /* orange */
    PAINT_PALETTE[7] = fb_pack_color(0x90, 0x60, 0x30);   /* brown */
}

/* A glossy gradient button with a crisp white outline (so it stays visibly
   a "button" even sitting on a busy/colored background like the green
   taskbar) - used for taskbar buttons, Paint/Notepad toolbar buttons, and
   (with different colors) the window control buttons. */
static void draw_gradient_button_rgb2(int32_t x, int32_t y, int32_t w, int32_t h, bool pressed,
                                       uint32_t outline_color,
                                       uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                       uint8_t bot_r, uint8_t bot_g, uint8_t bot_b) {
    fb_fill_rect(x, y, w, 1, outline_color);
    fb_fill_rect(x, y, 1, h, outline_color);
    fb_fill_rect(x, y + h - 1, w, 1, outline_color);
    fb_fill_rect(x + w - 1, y, 1, h, outline_color);
    if (pressed) {
        fb_fill_rect_vgradient(x + 1, y + 1, w - 2, h - 2, bot_r, bot_g, bot_b, top_r, top_g, top_b);
    } else {
        fb_fill_rect_vgradient(x + 1, y + 1, w - 2, h - 2, top_r, top_g, top_b, bot_r, bot_g, bot_b);
    }
}

static void draw_gradient_button_rgb(int32_t x, int32_t y, int32_t w, int32_t h, bool pressed,
                                      uint8_t top_r, uint8_t top_g, uint8_t top_b,
                                      uint8_t bot_r, uint8_t bot_g, uint8_t bot_b) {
    draw_gradient_button_rgb2(x, y, w, h, pressed, fb_pack_color(255, 255, 255),
                               top_r, top_g, top_b, bot_r, bot_g, bot_b);
}

/* The common case: a plain white/light-gray glossy button. */
static void draw_gradient_button(int32_t x, int32_t y, int32_t w, int32_t h, bool pressed) {
    draw_gradient_button_rgb(x, y, w, h, pressed, 255, 255, 255, 195, 195, 200);
}

static void draw_bevel(int32_t x, int32_t y, int32_t w, int32_t h, bool raised) {
    uint32_t outer = raised ? COL_FACE_LIGHT : COL_FACE_DARKSHADOW;
    uint32_t outer_opposite = raised ? COL_FACE_DARKSHADOW : COL_FACE_LIGHT;
    uint32_t inner = raised ? COL_FACE : COL_FACE_SHADOW;
    uint32_t inner_opposite = raised ? COL_FACE_SHADOW : COL_FACE;

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

void wm_init(void) {
    memset(windows, 0, sizeof(windows));
    z_count = 0;
    screen_w = (int32_t)fb_width();
    screen_h = (int32_t)fb_height();
    init_colors();
    serial_write("[wm] window manager initialized\n");
}

static void z_remove(int idx) {
    int w = 0;
    for (int r = 0; r < z_count; r++) {
        if (z_order[r] != idx) z_order[w++] = z_order[r];
    }
    z_count = w;
}

static void z_bring_to_front(int idx) {
    z_remove(idx);
    z_order[z_count++] = idx;
}

int wm_open_window_at(const char *title, uint32_t content_color, app_type_t app_type, const char *path) {
    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].in_use) continue;

        memset(&windows[i], 0, sizeof(window_t));
        windows[i].in_use = true;
        windows[i].minimized = false;
        windows[i].app_type = app_type;

        switch (app_type) {
            case APP_NOTEPAD:    windows[i].w = 340; windows[i].h = 260; break;
            case APP_CALCULATOR: windows[i].w = 190; windows[i].h = 260; break;
            case APP_PAINT:      windows[i].w = 320; windows[i].h = 260; break;
            case APP_EXPLORER:   windows[i].w = 320; windows[i].h = 260; break;
            case APP_BROWSER:    windows[i].w = 420; windows[i].h = 300; break;
            case APP_TERMINAL:   windows[i].w = 400; windows[i].h = 260; break;
            default:              windows[i].w = 320; windows[i].h = 220; break;
        }
        windows[i].x = 100 + i * 24;
        windows[i].y = 80 + i * 24;
        strncpy(windows[i].title, title, WM_TITLE_MAX - 1);
        windows[i].title[WM_TITLE_MAX - 1] = '\0';
        windows[i].content_color = content_color;

        if (app_type == APP_CALCULATOR) {
            strcpy(windows[i].calc_display, "0");
            windows[i].calc_accum = 0;
            windows[i].calc_pending_op = 0;
            windows[i].calc_fresh_entry = true;
        } else if (app_type == APP_NOTEPAD) {
            strncpy(windows[i].socks_path, "software/notepad/notepad.socks", sizeof(windows[i].socks_path) - 1);
            windows[i].socks_path[sizeof(windows[i].socks_path) - 1] = '\0';
            windows[i].socks_ready = false;
            windows[i].notepad_path[0] = '\0';
            if (path && path[0]) {
                strncpy(windows[i].notepad_path, path, sizeof(windows[i].notepad_path) - 1);
                windows[i].notepad_path[sizeof(windows[i].notepad_path) - 1] = '\0';
            }
        } else if (app_type == APP_SOCKS) {
            windows[i].socks_ready = false;
            windows[i].notepad_path[0] = '\0';
            if (path && path[0]) {
                strncpy(windows[i].socks_path, path, sizeof(windows[i].socks_path) - 1);
                windows[i].socks_path[sizeof(windows[i].socks_path) - 1] = '\0';
            }
        } else if (app_type == APP_EXPLORER) {
            strncpy(windows[i].explorer_path, (path && path[0]) ? path : "/", sizeof(windows[i].explorer_path) - 1);
            windows[i].explorer_cache_valid = false;
        } else if (app_type == APP_TERMINAL) {
            windows[i].term_output[0] = '\0';
            windows[i].term_output_len = 0;
            windows[i].term_cwd[0] = '\0';
            windows[i].term_input[0] = '\0';
            windows[i].term_input_len = 0;
        } else if (app_type == APP_PAINT) {
            int32_t cw = windows[i].w;
            int32_t ch = windows[i].h - TITLEBAR_HEIGHT - PAINT_TOOLBAR_H;
            windows[i].paint_canvas = (uint32_t*)kmalloc((size_t)cw * ch * sizeof(uint32_t));
            windows[i].paint_canvas_w = cw;
            windows[i].paint_canvas_h = ch;
            windows[i].paint_tool = PAINT_TOOL_BRUSH;
            windows[i].paint_color = fb_pack_color(0, 0, 0);
            windows[i].paint_last_x = -1;
            windows[i].paint_last_y = -1;
            windows[i].paint_shape_dragging = false;
            windows[i].paint_text_active = false;
            windows[i].paint_saving_as = false;
            windows[i].paint_path[0] = '\0';
            windows[i].paint_undo_canvas = NULL;
            if (windows[i].paint_canvas) {
                for (int32_t p = 0; p < cw * ch; p++) windows[i].paint_canvas[p] = 0xFFFFFFFF;
            }
            if (path && path[0] && windows[i].paint_canvas) {
                static uint8_t bmp_load_buf[NFS_MAX_FILE_SIZE];
                int n = nfs_read_path(path, bmp_load_buf, sizeof(bmp_load_buf));
                if (n > 0) {
                    int32_t loaded_w, loaded_h;
                    if (bmp_decode(bmp_load_buf, n, windows[i].paint_canvas, cw, ch, &loaded_w, &loaded_h)) {
                        strncpy(windows[i].paint_path, path, sizeof(windows[i].paint_path) - 1);
                    }
                }
            }
        } else if (app_type == APP_BROWSER) {
            strcpy(windows[i].browser_url, "");
            windows[i].browser_url_len = 0;
            windows[i].browser_editing_url = false;
            windows[i].browser_state = 0;
            windows[i].browser_conn = NULL;
            windows[i].browser_response_len = 0;
            windows[i].browser_history_count = 0;
            windows[i].browser_forward_count = 0;
            windows[i].browser_line_count = 0;
        }

        z_bring_to_front(i);
        return i;
    }
    return -1; /* no free window slots */
}

int wm_open_window(const char *title, uint32_t content_color, app_type_t app_type) {
    return wm_open_window_at(title, content_color, app_type, "");
}

static void close_button_rect(const window_t *w, int32_t *bx, int32_t *by) {
    *bx = w->x + w->w - BTN_SIZE - 3;
    *by = w->y + (TITLEBAR_HEIGHT - BTN_SIZE) / 2;
}

static void maximize_button_rect(const window_t *w, int32_t *bx, int32_t *by) {
    *bx = w->x + w->w - BTN_SIZE * 2 - 6;
    *by = w->y + (TITLEBAR_HEIGHT - BTN_SIZE) / 2;
}

static void minimize_button_rect(const window_t *w, int32_t *bx, int32_t *by) {
    *bx = w->x + w->w - BTN_SIZE * 3 - 9;
    *by = w->y + (TITLEBAR_HEIGHT - BTN_SIZE) / 2;
}

static bool point_in_rect(int32_t px, int32_t py, int32_t x, int32_t y, int32_t w, int32_t h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

/* --- Calculator button layout: shared by drawing and click hit-testing --- */
#define CALC_BTN_W 40
#define CALC_BTN_H 34
#define CALC_BTN_GAP 4
#define CALC_GRID_COLS 4
#define CALC_GRID_ROWS 5

static const char *calc_labels[CALC_GRID_ROWS][CALC_GRID_COLS] = {
    {"C",  "",   "",   "" },
    {"7",  "8",  "9",  "/"},
    {"4",  "5",  "6",  "*"},
    {"1",  "2",  "3",  "-"},
    {"0",  ".",  "=",  "+"},
};

static void calc_button_rect(const window_t *w, int row, int col, int32_t *bx, int32_t *by) {
    int32_t content_y = w->y + TITLEBAR_HEIGHT;
    *bx = w->x + 6 + col * (CALC_BTN_W + CALC_BTN_GAP);
    *by = content_y + 40 + row * (CALC_BTN_H + CALC_BTN_GAP);
}

static void calc_append_digit(window_t *w, char d) {
    if (w->calc_fresh_entry) {
        w->calc_display[0] = d;
        w->calc_display[1] = '\0';
        w->calc_fresh_entry = false;
    } else {
        int32_t len = (int32_t)strlen(w->calc_display);
        if (len < (int32_t)sizeof(w->calc_display) - 1) {
            w->calc_display[len] = d;
            w->calc_display[len + 1] = '\0';
        }
    }
}

/* Extremely small integer/decimal string -> fixed-point int64_t parser
   (no libc, no floats - SSE is disabled in this kernel build). Fixed-point
   values are the real value multiplied by CALC_SCALE (4 decimal digits). */
#define CALC_SCALE 10000

static int64_t calc_parse(const char *s) {
    int64_t int_part = 0;
    int64_t frac_part = 0;
    int64_t frac_scale = 1;
    bool in_frac = false;
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    for (; *s; s++) {
        if (*s == '.') { in_frac = true; continue; }
        if (*s < '0' || *s > '9') continue;
        if (in_frac) {
            if (frac_scale < CALC_SCALE) {
                frac_scale *= 10;
                frac_part = frac_part * 10 + (*s - '0');
            }
        } else {
            int_part = int_part * 10 + (*s - '0');
        }
    }
    int64_t scaled = int_part * CALC_SCALE + (frac_part * CALC_SCALE) / frac_scale;
    return neg ? -scaled : scaled;
}

static void calc_format(window_t *w, int64_t scaled) {
    bool neg = scaled < 0;
    if (neg) scaled = -scaled;

    int64_t int_part = scaled / CALC_SCALE;
    int64_t frac_part = scaled % CALC_SCALE;

    char buf[24];
    int i = 0;
    if (neg) buf[i++] = '-';

    char digits[24]; int di = 0;
    if (int_part == 0) digits[di++] = '0';
    while (int_part > 0 && di < 20) {
        digits[di++] = (char)('0' + (int_part % 10));
        int_part /= 10;
    }
    while (di > 0) buf[i++] = digits[--di];

    if (frac_part > 0) {
        buf[i++] = '.';
        char fbuf[5];
        int64_t f = frac_part;
        for (int k = 3; k >= 0; k--) { fbuf[k] = (char)('0' + f % 10); f /= 10; }
        int end = 4;
        while (end > 1 && fbuf[end - 1] == '0') end--;
        for (int k = 0; k < end; k++) buf[i++] = fbuf[k];
    }
    buf[i] = '\0';
    strncpy(w->calc_display, buf, sizeof(w->calc_display) - 1);
    w->calc_display[sizeof(w->calc_display) - 1] = '\0';
}

static int64_t calc_apply(char op, int64_t a, int64_t b) {
    switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return (a * b) / CALC_SCALE;
        case '/': return (b != 0) ? (a * CALC_SCALE) / b : 0;
        default:  return b;
    }
}

static void calc_handle_button(window_t *w, const char *label) {
    if (label[0] == '\0') return;

    if (label[0] >= '0' && label[0] <= '9') {
        calc_append_digit(w, label[0]);
    } else if (label[0] == '.') {
        if (!w->calc_fresh_entry) {
            /* only add a decimal point if there isn't one already */
            bool has_dot = false;
            for (const char *p = w->calc_display; *p; p++) if (*p == '.') has_dot = true;
            if (!has_dot) calc_append_digit(w, '.');
        } else {
            strcpy(w->calc_display, "0.");
            w->calc_fresh_entry = false;
        }
    } else if (label[0] == 'C') {
        w->calc_accum = 0;
        w->calc_pending_op = 0;
        w->calc_fresh_entry = true;
        strcpy(w->calc_display, "0");
    } else if (label[0] == '=' ) {
        int64_t cur = calc_parse(w->calc_display);
        int64_t result = calc_apply(w->calc_pending_op, w->calc_accum, cur);
        if (!w->calc_pending_op) result = cur;
        calc_format(w, result);
        w->calc_accum = result;
        w->calc_pending_op = 0;
        w->calc_fresh_entry = true;
    } else { /* + - * / */
        int64_t cur = calc_parse(w->calc_display);
        if (w->calc_pending_op) {
            int64_t result = calc_apply(w->calc_pending_op, w->calc_accum, cur);
            w->calc_accum = result;
            calc_format(w, result);
        } else {
            w->calc_accum = cur;
        }
        w->calc_pending_op = label[0];
        w->calc_fresh_entry = true;
    }
}

/* --- Per-app content rendering --- */

#define NOTEPAD_TOOLBAR_H 24

static void notepad_save_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 4; *by = w->y + TITLEBAR_HEIGHT + 3; *bw = 56; *bh = 18;
}
static void notepad_saveas_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 64; *by = w->y + TITLEBAR_HEIGHT + 3; *bw = 80; *bh = 18;
}

static void draw_notepad(const window_t *w) {
    int32_t cx = w->x, cy = w->y + TITLEBAR_HEIGHT;
    int32_t cw = w->w, ch = w->h - TITLEBAR_HEIGHT;
    fb_fill_rect(cx, cy, cw, NOTEPAD_TOOLBAR_H, COL_FACE);
    fb_fill_rect(cx, cy + NOTEPAD_TOOLBAR_H - 1, cw, 1, COL_FACE_SHADOW);

    int32_t bx, by, bw, bh;
    notepad_save_button_rect(w, &bx, &by, &bw, &bh);
    draw_gradient_button(bx, by, bw, bh, false);
    font_draw_string(bx + (bw - font_string_width("SAVE", FONT_SCALE)) / 2,
                      by + (bh - font_char_height(FONT_SCALE)) / 2, "SAVE", COL_TEXT_BLACK, FONT_SCALE);

    notepad_saveas_button_rect(w, &bx, &by, &bw, &bh);
    draw_gradient_button(bx, by, bw, bh, false);
    font_draw_string(bx + (bw - font_string_width("SAVE AS", FONT_SCALE)) / 2,
                      by + (bh - font_char_height(FONT_SCALE)) / 2, "SAVE AS", COL_TEXT_BLACK, FONT_SCALE);

    const char *shown_path = w->notepad_path[0] ? w->notepad_path : "(UNSAVED)";
    font_draw_string(cx + 152, cy + 3 + (NOTEPAD_TOOLBAR_H - 6 - font_char_height(FONT_SCALE)) / 2,
                      shown_path, COL_TEXT_BLACK, FONT_SCALE);

    int32_t body_y = cy + NOTEPAD_TOOLBAR_H;
    int32_t body_h = ch - NOTEPAD_TOOLBAR_H;

    if (w->notepad_saving_as) {
        int32_t bar_h = 22;
        fb_fill_rect(cx + 2, body_y + 2, cw - 4, bar_h, COL_WHITE);
        draw_bevel(cx + 2, body_y + 2, cw - 4, bar_h, false);
        const char *disp = w->notepad_saveas_len > 0 ? w->notepad_saveas_buf : "TYPE PATH, E.G. Documents/notes.txt";
        font_draw_string(cx + 6, body_y + 2 + (bar_h - font_char_height(FONT_SCALE)) / 2, disp, COL_TEXT_BLACK, FONT_SCALE);
        int32_t tw = font_string_width(w->notepad_saveas_buf, FONT_SCALE);
        fb_fill_rect(cx + 6 + tw + 2, body_y + 5, 2, bar_h - 6, COL_TEXT_BLACK);
        body_y += bar_h + 4;
        body_h -= bar_h + 4;
    }

    fb_fill_rect(cx, body_y, cw, body_h, COL_WHITE);
    draw_bevel(cx, body_y, cw, body_h, false);

    int32_t line_h = font_char_height(FONT_SCALE) + 3;
    int32_t tx = cx + 4, ty = body_y + 4;
    char line_buf[64];
    int32_t line_len = 0;

    for (int32_t i = 0; i <= w->text_len; i++) {
        char c = (i < w->text_len) ? w->text[i] : '\n';
        if (c == '\n') {
            line_buf[line_len] = '\0';
            font_draw_string(tx, ty, line_buf, COL_TEXT_BLACK, FONT_SCALE);
            ty += line_h;
            line_len = 0;
        } else if (line_len < (int32_t)sizeof(line_buf) - 1) {
            line_buf[line_len++] = c;
        }
    }
}

static void draw_calculator(const window_t *w) {
    int32_t cx = w->x, cy = w->y + TITLEBAR_HEIGHT;
    int32_t cw = w->w, ch = w->h - TITLEBAR_HEIGHT;
    fb_fill_rect(cx, cy, cw, ch, COL_FACE);

    /* Display panel */
    fb_fill_rect(cx + 6, cy + 6, cw - 12, 28, COL_WHITE);
    draw_bevel(cx + 6, cy + 6, cw - 12, 28, false);
    int32_t tw = font_string_width(w->calc_display, 2);
    font_draw_string(cx + cw - 10 - tw, cy + 6 + (28 - font_char_height(2)) / 2,
                      w->calc_display, COL_TEXT_BLACK, 2);

    /* Buttons */
    for (int row = 0; row < CALC_GRID_ROWS; row++) {
        for (int col = 0; col < CALC_GRID_COLS; col++) {
            const char *label = calc_labels[row][col];
            if (label[0] == '\0') continue;
            int32_t bx, by;
            calc_button_rect(w, row, col, &bx, &by);
            draw_gradient_button(bx, by, CALC_BTN_W, CALC_BTN_H, false);
            int32_t lw = font_string_width(label, 2);
            font_draw_string(bx + (CALC_BTN_W - lw) / 2, by + (CALC_BTN_H - font_char_height(2)) / 2,
                              label, COL_TEXT_BLACK, 2);
        }
    }
}

#define PAINT_TOOL_COUNT 7

static void paint_tool_button_rect(const window_t *w, int index, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 4 + index * 20;
    *by = w->y + TITLEBAR_HEIGHT + 3;
    *bw = 18;
    *bh = 20;
}

static void paint_undo_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 4 + PAINT_TOOL_COUNT * 20 + 6;
    *by = w->y + TITLEBAR_HEIGHT + 3;
    *bw = 24;
    *bh = 20;
}

static void paint_save_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 4 + PAINT_TOOL_COUNT * 20 + 6 + 24 + 6;
    *by = w->y + TITLEBAR_HEIGHT + 3;
    *bw = 40;
    *bh = 20;
}

static void paint_saveas_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 4 + PAINT_TOOL_COUNT * 20 + 6 + 24 + 6 + 40 + 4;
    *by = w->y + TITLEBAR_HEIGHT + 3;
    *bw = 56;
    *bh = 20;
}

static void paint_swatch_rect(const window_t *w, int index, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 4 + index * (PAINT_SWATCH_SIZE + 3);
    *by = w->y + TITLEBAR_HEIGHT + 27;
    *bw = PAINT_SWATCH_SIZE;
    *bh = PAINT_SWATCH_SIZE;
}

static void draw_paint(const window_t *w) {
    int32_t cx = w->x, cy = w->y + TITLEBAR_HEIGHT;
    int32_t full_w = w->w;

    /* Toolbar */
    fb_fill_rect(cx, cy, full_w, PAINT_TOOLBAR_H, COL_FACE);
    fb_fill_rect(cx, cy + PAINT_TOOLBAR_H - 1, full_w, 1, COL_FACE_SHADOW);

    const char *tool_labels[PAINT_TOOL_COUNT] = { "B", "E", "F", "L", "R", "O", "T" };
    for (int i = 0; i < PAINT_TOOL_COUNT; i++) {
        int32_t bx, by, bw, bh;
        paint_tool_button_rect(w, i, &bx, &by, &bw, &bh);
        draw_gradient_button(bx, by, bw, bh, w->paint_tool == i);
        int32_t lw = font_string_width(tool_labels[i], 1);
        font_draw_string(bx + (bw - lw) / 2, by + (bh - font_char_height(1)) / 2, tool_labels[i], COL_TEXT_BLACK, 1);
    }

    int32_t ubx, uby, ubw, ubh;
    paint_undo_button_rect(w, &ubx, &uby, &ubw, &ubh);
    draw_gradient_button(ubx, uby, ubw, ubh, false);
    font_draw_string(ubx + (ubw - font_string_width("U", 1)) / 2, uby + (ubh - font_char_height(1)) / 2, "U", COL_TEXT_BLACK, 1);

    int32_t sbx, sby, sbw, sbh;
    paint_save_button_rect(w, &sbx, &sby, &sbw, &sbh);
    draw_gradient_button(sbx, sby, sbw, sbh, false);
    font_draw_string(sbx + (sbw - font_string_width("SAVE", 1)) / 2, sby + (sbh - font_char_height(1)) / 2, "SAVE", COL_TEXT_BLACK, 1);

    int32_t abx, aby, abw, abh;
    paint_saveas_button_rect(w, &abx, &aby, &abw, &abh);
    draw_gradient_button(abx, aby, abw, abh, false);
    font_draw_string(abx + (abw - font_string_width("SAVE AS", 1)) / 2, aby + (abh - font_char_height(1)) / 2, "SAVE AS", COL_TEXT_BLACK, 1);

    for (int i = 0; i < PAINT_PALETTE_COUNT; i++) {
        int32_t bx, by, bw, bh;
        paint_swatch_rect(w, i, &bx, &by, &bw, &bh);
        uint8_t sr, sg, sb;
        fb_unpack_color(PAINT_PALETTE[i], &sr, &sg, &sb);
        uint8_t lr = (uint8_t)(sr + (255 - sr) * 3 / 5);
        uint8_t lg = (uint8_t)(sg + (255 - sg) * 3 / 5);
        uint8_t lb = (uint8_t)(sb + (255 - sb) * 3 / 5);
        uint8_t dr = (uint8_t)(sr * 3 / 5);
        uint8_t dg = (uint8_t)(sg * 3 / 5);
        uint8_t db = (uint8_t)(sb * 3 / 5);
        bool selected = (PAINT_PALETTE[i] == w->paint_color);
        draw_gradient_button_rgb2(bx, by, bw, bh, false,
                                   selected ? fb_pack_color(30, 70, 160) : fb_pack_color(255, 255, 255),
                                   lr, lg, lb, dr, dg, db);
    }

    if (w->paint_saving_as) {
        int32_t path_y = cy + PAINT_TOOLBAR_H - 24;
        fb_fill_rect(cx + 100, path_y, full_w - 104, 20, COL_WHITE);
        draw_bevel(cx + 100, path_y, full_w - 104, 20, false);
        const char *disp = w->paint_saveas_len > 0 ? w->paint_saveas_buf : "PATH, E.G. Documents/pic.bmp";
        font_draw_string(cx + 104, path_y + 3, disp, COL_TEXT_BLACK, 1);
    }

    /* Canvas */
    int32_t canvas_y = cy + PAINT_TOOLBAR_H;
    int32_t ch = w->h - TITLEBAR_HEIGHT - PAINT_TOOLBAR_H;
    draw_bevel(cx, canvas_y, full_w, ch, false);

    if (!w->paint_canvas) {
        fb_fill_rect(cx, canvas_y, full_w, ch, COL_WHITE);
        return;
    }
    {
        int32_t blit_w = w->paint_canvas_w < full_w ? w->paint_canvas_w : full_w;
        int32_t blit_h = w->paint_canvas_h < ch ? w->paint_canvas_h : ch;
        fb_blit_packed(cx, canvas_y, blit_w, blit_h, w->paint_canvas, w->paint_canvas_w);
    }

    if (w->paint_text_active) {
        int32_t tx = cx + w->paint_text_x, ty = canvas_y + w->paint_text_y;
        fb_fill_rect(tx, ty, 2, font_char_height(1), fb_pack_color(0, 0, 0));
    }
}

static bool explorer_is_dir[EXPLORER_MAX_FILES];

/* Joins a directory path and a name into a child path, e.g. ("Documents","a.txt") -> "Documents/a.txt".
   An empty/root dir_path just yields the bare name. */
static void path_join(const char *dir_path, const char *name, char *out, size_t out_size) {
    if (dir_path == NULL || dir_path[0] == '\0' || strcmp(dir_path, "/") == 0) {
        strncpy(out, name, out_size - 1);
        out[out_size - 1] = '\0';
    } else {
        strncpy(out, dir_path, out_size - 1);
        out[out_size - 1] = '\0';
        size_t len = strlen(out);
        if (len < out_size - 1) { out[len] = '/'; out[len + 1] = '\0'; }
        strncat(out, name, out_size - strlen(out) - 1);
    }
}

/* Computes the parent of a path, e.g. "Documents/sub" -> "Documents", "Documents" -> "" (root). */
static void path_parent(const char *path, char *out, size_t out_size) {
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last_slash = p;
    if (!last_slash) {
        out[0] = '\0';
    } else {
        size_t len = (size_t)(last_slash - path);
        if (len >= out_size) len = out_size - 1;
        memcpy(out, path, len);
        out[len] = '\0';
    }
}

/* Finds an unused "Base Name.ext" / "Base Name 2.ext" / ... within dir_path. */
static void path_make_unique(const char *dir_path, const char *base, const char *ext, char *out, size_t out_size) {
    for (int n = 1; n < 200; n++) {
        char candidate[NFS_MAX_NAME];
        if (n == 1) {
            int len = 0;
            const char *p = base; while (*p && len < (int)sizeof(candidate)-1) candidate[len++]=*p++;
            candidate[len] = '\0';
            strncat(candidate, ext, sizeof(candidate) - strlen(candidate) - 1);
        } else {
            char numbuf[8]; int ni=0; int nn=n;
            char tmp[8]; int ti=0;
            if (nn==0) tmp[ti++]='0';
            while (nn>0) { tmp[ti++] = (char)('0' + nn%10); nn/=10; }
            while (ti>0) numbuf[ni++]=tmp[--ti];
            numbuf[ni]='\0';
            strcpy(candidate, base);
            strncat(candidate, " ", sizeof(candidate) - strlen(candidate) - 1);
            strncat(candidate, numbuf, sizeof(candidate) - strlen(candidate) - 1);
            strncat(candidate, ext, sizeof(candidate) - strlen(candidate) - 1);
        }
        char full[NFS_MAX_PATH];
        path_join(dir_path, candidate, full, sizeof(full));
        if (!nfs_exists_path(full)) {
            strncpy(out, candidate, out_size - 1);
            out[out_size - 1] = '\0';
            return;
        }
    }
    strcpy(out, "New Item");
}

static void explorer_collect(const char *name, uint32_t size, bool is_dir) {
    if (strcmp(name, "SYSTEM.CFG") == 0) return; /* internal marker file, not meant to be seen/touched */
    if (explorer_count < EXPLORER_MAX_FILES) {
        strncpy(explorer_names[explorer_count], name, 31);
        explorer_names[explorer_count][31] = '\0';
        explorer_sizes[explorer_count] = size;
        explorer_is_dir[explorer_count] = is_dir;
        explorer_count++;
    }
}

/* Computes the on-screen rect for grid item `index` within an icon-grid area
   (used identically by Explorer's file list and the desktop icons). */
static void grid_item_rect(int32_t area_x, int32_t area_y, int32_t area_w, int col_w, int row_h,
                            int index, int32_t *ix, int32_t *iy) {
    int cols = area_w / col_w;
    if (cols < 1) cols = 1;
    int col = index % cols;
    int row = index / cols;
    *ix = area_x + col * col_w;
    *iy = area_y + row * row_h;
}

/* Inline rename state (typing directly over an icon/item's name) - declared
   here (rather than right next to start_rename/finish_rename further down)
   so draw_explorer can show the in-progress edit, same as desktop icons. */
static bool renaming_active = false;
static int  renaming_window = -1;     /* -1 = desktop */
static char renaming_dir_path[NFS_MAX_PATH];
static char renaming_old_name[NFS_MAX_NAME];
static char renaming_buf[NFS_MAX_NAME];
static int32_t renaming_len = 0;

static void draw_explorer(const window_t *w) {
    int32_t cx = w->x, cy = w->y + TITLEBAR_HEIGHT;
    int32_t cw = w->w, ch = w->h - TITLEBAR_HEIGHT;
    fb_fill_rect(cx, cy, cw, ch, COL_FACE);

    /* Address bar shows the current path */
    int32_t addr_h = 22;
    fb_fill_rect(cx + 2, cy + 2, cw - 4, addr_h, COL_WHITE);
    draw_bevel(cx + 2, cy + 2, cw - 4, addr_h, false);
    icon_draw(cx + 5, cy + 4, ICON_DISK, 1);
    const char *path_display = (w->explorer_path[0] == '\0' || strcmp(w->explorer_path, "/") == 0)
                                ? "LOCAL DISK (C:)" : w->explorer_path;
    font_draw_string(cx + 24, cy + 2 + (addr_h - font_char_height(FONT_SCALE)) / 2,
                      path_display, COL_TEXT_BLACK, FONT_SCALE);

    /* File area */
    int32_t list_y = cy + 2 + addr_h + 2;
    int32_t status_h = 16;
    int32_t list_h = ch - addr_h - 4 - status_h - 2;
    fb_fill_rect(cx + 2, list_y, cw - 4, list_h, COL_WHITE);
    draw_bevel(cx + 2, list_y, cw - 4, list_h, false);

    explorer_count = w->explorer_cache_count;
    for (int32_t ci = 0; ci < w->explorer_cache_count && ci < EXPLORER_MAX_FILES; ci++) {
        strncpy(explorer_names[ci], w->explorer_cache_names[ci], 31);
        explorer_names[ci][31] = '\0';
        explorer_sizes[ci] = w->explorer_cache_sizes[ci];
        explorer_is_dir[ci] = w->explorer_cache_is_dir[ci];
    }
    bool at_root = (w->explorer_path[0] == '\0' || strcmp(w->explorer_path, "/") == 0);

    int32_t col_w = 70, row_h = 50;
    int32_t first_index = at_root ? 0 : 1; /* slot 0 reserved for ".." if not at root */

    if (!at_root) {
        int32_t ix, iy;
        grid_item_rect(cx + 8, list_y + 6, cw - 12, col_w, row_h, 0, &ix, &iy);
        icon_draw(ix, iy, ICON_FOLDER, 2);
        font_draw_string(ix, iy + 36, "..", COL_TEXT_BLACK, FONT_SCALE);
    }

    for (int i = 0; i < explorer_count; i++) {
        int32_t ix, iy;
        grid_item_rect(cx + 8, list_y + 6, cw - 12, col_w, row_h, first_index + i, &ix, &iy);
        icon_draw(ix, iy, explorer_is_dir[i] ? ICON_FOLDER : ICON_FILE, 2);

        int win_idx = (int)(w - windows);
        const char *label = explorer_names[i];
        if (renaming_active && renaming_window == win_idx && strcmp(renaming_old_name, explorer_names[i]) == 0) {
            label = renaming_buf;
        }
        char short_name[10];
        strncpy(short_name, label, 9);
        short_name[9] = '\0';
        font_draw_string(ix, iy + 36, short_name, COL_TEXT_BLACK, FONT_SCALE);
    }

    if (explorer_count == 0 && at_root == false) {
        font_draw_string(cx + 8 + col_w + 4, list_y + 8, "EMPTY", COL_TEXT_BLACK, FONT_SCALE);
    } else if (explorer_count == 0) {
        font_draw_string(cx + 10, list_y + 8, "THIS FOLDER IS EMPTY", COL_TEXT_BLACK, FONT_SCALE);
    }

    /* Status bar */
    int32_t status_y = cy + ch - status_h - 2;
    fb_fill_rect(cx + 2, status_y, cw - 4, status_h, COL_FACE);
    draw_bevel(cx + 2, status_y, cw - 4, status_h, false);
    char status_text[32];
    int n = explorer_count;
    status_text[0] = (char)('0' + (n / 10) % 10);
    status_text[1] = (char)('0' + n % 10);
    status_text[2] = ' ';
    strcpy(status_text + 3, "OBJECT(S)");
    if (n < 10) { /* skip the leading zero for single digits */
        strcpy(status_text, status_text + 1);
    }
    font_draw_string(cx + 6, status_y + (status_h - font_char_height(FONT_SCALE)) / 2, status_text, COL_TEXT_BLACK, FONT_SCALE);
}

/* Iterative flood fill (bucket tool) - avoids recursion so it can't blow the
   kernel stack even on a large canvas. */
static void paint_flood_fill(window_t *w, int32_t start_x, int32_t start_y, uint32_t new_color) {
    if (!w->paint_canvas) return;
    int32_t cw = w->paint_canvas_w, ch = w->paint_canvas_h;
    if (start_x < 0 || start_x >= cw || start_y < 0 || start_y >= ch) return;

    uint32_t target = w->paint_canvas[start_y * cw + start_x];
    if (target == new_color) return;

    int32_t *stack = (int32_t*)kmalloc((size_t)cw * ch * sizeof(int32_t));
    if (!stack) return;
    int32_t sp = 0;
    stack[sp++] = start_y * cw + start_x;

    while (sp > 0) {
        int32_t idx = stack[--sp];
        int32_t x = idx % cw, y = idx / cw;
        if (x < 0 || x >= cw || y < 0 || y >= ch) continue;
        if (w->paint_canvas[y * cw + x] != target) continue;

        w->paint_canvas[y * cw + x] = new_color;
        if (x > 0)      stack[sp++] = y * cw + (x - 1);
        if (x < cw - 1) stack[sp++] = y * cw + (x + 1);
        if (y > 0)      stack[sp++] = (y - 1) * cw + x;
        if (y < ch - 1) stack[sp++] = (y + 1) * cw + x;
    }

    kfree(stack);
}

/* Draws a brush stroke (3x3 stamp) along the line from (x0,y0) to (x1,y1) so
   fast mouse movement doesn't leave gaps between sampled positions. */
static void paint_draw_line(window_t *w, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    if (!w->paint_canvas) return;
    int32_t cw = w->paint_canvas_w, ch = w->paint_canvas_h;

    int32_t dx = x1 - x0; if (dx < 0) dx = -dx;
    int32_t dy = y1 - y0; if (dy < 0) dy = -dy;
    int32_t sx = (x0 < x1) ? 1 : -1;
    int32_t sy = (y0 < y1) ? 1 : -1;
    int32_t err = dx - dy;
    int32_t x = x0, y = y0;

    for (;;) {
        for (int32_t oy = -1; oy <= 1; oy++) {
            for (int32_t ox = -1; ox <= 1; ox++) {
                int32_t px = x + ox, py = y + oy;
                if (px >= 0 && px < cw && py >= 0 && py < ch) {
                    w->paint_canvas[py * cw + px] = color;
                }
            }
        }
        if (x == x1 && y == y1) break;
        int32_t e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx)  { err += dx; y += sy; }
    }
}

static void paint_draw_rect(window_t *w, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
    paint_draw_line(w, x0, y0, x1, y0, color);
    paint_draw_line(w, x0, y1, x1, y1, color);
    paint_draw_line(w, x0, y0, x0, y1, color);
    paint_draw_line(w, x1, y0, x1, y1, color);
}

/* Precomputed sin(deg)*1000 for 0-90 degrees, used to draw ellipses without
   floating point (SSE is disabled in this kernel) or a runtime trig call. */
static const int16_t sin_table_deg[91] = {
    0,17,35,52,70,87,105,122,139,156,174,191,208,225,242,259,276,292,309,326,
    342,358,375,391,407,423,438,454,469,485,500,515,530,545,559,574,588,602,
    616,629,643,656,669,682,695,707,719,731,743,755,766,777,788,799,809,819,
    829,839,848,857,866,875,883,891,899,906,914,921,927,934,940,946,951,956,
    961,966,970,974,978,982,985,988,990,993,995,996,998,999,999,1000,1000,
    1000,1000
};
static int32_t isin1000(int32_t deg) {
    deg %= 360; if (deg < 0) deg += 360;
    if (deg <= 90) return sin_table_deg[deg];
    if (deg <= 180) return sin_table_deg[180 - deg];
    if (deg <= 270) return -sin_table_deg[deg - 180];
    return -sin_table_deg[360 - deg];
}
static int32_t icos1000(int32_t deg) { return isin1000(deg + 90); }

static void paint_draw_ellipse(window_t *w, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t color) {
    if (x1 < x0) { int32_t t = x0; x0 = x1; x1 = t; }
    if (y1 < y0) { int32_t t = y0; y0 = y1; y1 = t; }
    int32_t cx0 = (x0 + x1) / 2, cy0 = (y0 + y1) / 2;
    int32_t rx = (x1 - x0) / 2, ry = (y1 - y0) / 2;
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;

    int32_t px = -1, py = -1;
    for (int32_t deg = 0; deg <= 360; deg += 6) {
        int32_t nx = cx0 + (rx * isin1000(deg)) / 1000;
        int32_t ny = cy0 - (ry * icos1000(deg)) / 1000;
        if (px >= 0) paint_draw_line(w, px, py, nx, ny, color);
        px = nx; py = ny;
    }
}

/* Draws one character directly into a canvas buffer (not the framebuffer) - used by Paint's text tool. */
static void paint_draw_char_to_canvas(uint32_t *canvas, int32_t cw, int32_t ch_h, int32_t x, int32_t y, char c, uint32_t color) {
    uint8_t bitmap[7][5];
    font_get_glyph_bitmap(c, bitmap);
    for (int32_t ry = 0; ry < 7; ry++) {
        for (int32_t rx = 0; rx < 5; rx++) {
            if (!bitmap[ry][rx]) continue;
            int32_t px = x + rx, py = y + ry;
            if (px >= 0 && px < cw && py >= 0 && py < ch_h) canvas[py * cw + px] = color;
        }
    }
}

static const char *find_substr(const char *haystack, int32_t hlen, const char *needle) {
    int32_t nlen = (int32_t)strlen(needle);
    if (nlen == 0 || hlen < nlen) return NULL;
    for (int32_t i = 0; i <= hlen - nlen; i++) {
        bool match = true;
        for (int32_t j = 0; j < nlen; j++) {
            if (haystack[i + j] != needle[j]) { match = false; break; }
        }
        if (match) return haystack + i;
    }
    return NULL;
}

static void browser_draw_wrapped_text(int32_t x, int32_t y, int32_t max_w, int32_t max_y,
                                       const char *text, int32_t len, uint32_t color) {
    int32_t chars_per_line = max_w / ((5 + 1) * FONT_SCALE);
    if (chars_per_line < 1) chars_per_line = 1;
    if (chars_per_line > 120) chars_per_line = 120;
    int32_t line_h = font_char_height(FONT_SCALE) + 2;
    char line_buf[128];
    int32_t col = 0;
    int32_t cur_y = y;

    for (int32_t i = 0; i <= len; i++) {
        char c = (i < len) ? text[i] : '\n';
        bool flush = (c == '\n') || (col >= chars_per_line);
        if (flush) {
            line_buf[col] = '\0';
            if (cur_y < max_y) font_draw_string(x, cur_y, line_buf, color, FONT_SCALE);
            cur_y += line_h;
            col = 0;
            if (c == '\n') continue;
        }
        if (c != '\r' && col < (int32_t)sizeof(line_buf) - 1) line_buf[col++] = c;
        if (cur_y >= max_y) break;
    }
}

static bool parse_dotted_quad(const char *s, int32_t len, uint32_t *ip_out) {
    int32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    int32_t i = 0;
    bool any_digit = false;

    while (i < len && part < 4) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            parts[part] = parts[part] * 10 + (c - '0');
            any_digit = true;
        } else if (c == '.') {
            part++;
            any_digit = false;
        } else {
            return false; /* not a dotted-quad - has letters or other chars */
        }
        i++;
    }
    if (part != 3 || !any_digit || i != len) return false;

    *ip_out = MAKE_IP(parts[0], parts[1], parts[2], parts[3]);
    return true;
}

/* Splits a typed address into host (IP or hostname), port, and path. */
static void browser_split_url(const char *url, char *host_out, size_t host_out_size,
                               uint16_t *port_out, char *path_out, size_t path_out_size) {
    const char *p = url;
    size_t host_len = 0;
    while (*p && *p != ':' && *p != '/' && host_len < host_out_size - 1) {
        host_out[host_len++] = *p++;
    }
    host_out[host_len] = '\0';

    uint16_t port = 80;
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = (uint16_t)(port * 10 + (*p - '0')); p++; }
    }
    *port_out = port;

    if (*p == '/') {
        strncpy(path_out, p, path_out_size - 1);
        path_out[path_out_size - 1] = '\0';
    } else {
        strcpy(path_out, "/");
    }
}

static void browser_push_history(window_t *w, const char *url) {
    if (w->browser_history_count > 0 &&
        strcmp(w->browser_history[w->browser_history_count - 1], url) == 0) {
        return; /* don't duplicate consecutive identical entries (e.g. re-navigating) */
    }
    if (w->browser_history_count >= 8) {
        for (int i = 1; i < 8; i++) strcpy(w->browser_history[i - 1], w->browser_history[i]);
        w->browser_history_count = 7;
    }
    strncpy(w->browser_history[w->browser_history_count], url, 63);
    w->browser_history[w->browser_history_count][63] = '\0';
    w->browser_history_count++;
}

static void browser_navigate_to(window_t *w, const char *url, bool record_history, bool clear_forward) {
    char host[48];
    uint16_t port;
    char path[64];
    browser_split_url(url, host, sizeof(host), &port, path, sizeof(path));

    uint32_t ip;
    if (!parse_dotted_quad(host, (int32_t)strlen(host), &ip)) {
        w->browser_state = 1; /* show "connecting" while DNS resolves too */
        if (!dns_resolve(host, &ip)) {
            w->browser_state = 4;
            strcpy(w->browser_response, "COULD NOT RESOLVE THAT ADDRESS (DNS LOOKUP FAILED)");
            w->browser_response_len = (int32_t)strlen(w->browser_response);
            return;
        }
    }

    if (w->browser_conn) {
        tcp_close((tcp_connection_t*)w->browser_conn);
        w->browser_conn = NULL;
    }

    w->browser_target_ip = ip;
    w->browser_target_port = port;
    strncpy(w->browser_path, path, sizeof(w->browser_path) - 1);
    w->browser_response_len = 0;
    w->browser_response[0] = '\0';
    w->browser_line_count = 0;
    w->browser_conn = (void*)tcp_connect(ip, port);
    w->browser_state = w->browser_conn ? 1 : 4;

    if (record_history) browser_push_history(w, url);
    if (clear_forward) w->browser_forward_count = 0;
}

static void browser_navigate(window_t *w) {
    browser_navigate_to(w, w->browser_url, true, true);
}

static void browser_go_back(window_t *w) {
    if (w->browser_history_count < 2) return; /* nothing to go back to */
    if (w->browser_forward_count < 8) {
        strncpy(w->browser_forward[w->browser_forward_count], w->browser_history[w->browser_history_count - 1], 63);
        w->browser_forward[w->browser_forward_count][63] = '\0';
        w->browser_forward_count++;
    }
    w->browser_history_count--;
    const char *prev = w->browser_history[w->browser_history_count - 1];
    strncpy(w->browser_url, prev, sizeof(w->browser_url) - 1);
    w->browser_url[sizeof(w->browser_url) - 1] = '\0';
    w->browser_url_len = (int32_t)strlen(w->browser_url);
    browser_navigate_to(w, prev, false, false); /* don't push new history, don't clear forward */
}

static void browser_go_forward(window_t *w) {
    if (w->browser_forward_count < 1) return;
    w->browser_forward_count--;
    const char *next = w->browser_forward[w->browser_forward_count];
    strncpy(w->browser_url, next, sizeof(w->browser_url) - 1);
    w->browser_url[sizeof(w->browser_url) - 1] = '\0';
    w->browser_url_len = (int32_t)strlen(w->browser_url);
    browser_navigate_to(w, next, true, false); /* record it, but don't wipe the rest of the forward stack */
}

static void browser_decode_entities(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src == '&') {
            if (strncmp(src, "&amp;", 5) == 0)  { *dst++ = '&';  src += 5; continue; }
            if (strncmp(src, "&lt;", 4) == 0)   { *dst++ = '<';  src += 4; continue; }
            if (strncmp(src, "&gt;", 4) == 0)   { *dst++ = '>';  src += 4; continue; }
            if (strncmp(src, "&quot;", 6) == 0) { *dst++ = '"';  src += 6; continue; }
            if (strncmp(src, "&nbsp;", 6) == 0) { *dst++ = ' '; src += 6; continue; }
            if (strncmp(src, "&apos;", 6) == 0) { *dst++ = '\''; src += 6; continue; }
            if (strncmp(src, "&#39;", 5) == 0)  { *dst++ = '\''; src += 5; continue; }
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* Very small HTML "renderer": strips tags down to a sequence of lines, each
   either plain text, a heading (drawn bigger), or an entire clickable link
   (the whole line is one hyperlink - simpler than tracking mixed inline
   spans, at the cost of always putting links on their own line). No CSS,
   no JS, no images - just enough structure to make text readable and
   links clickable instead of showing raw tag soup. */
static void browser_parse_html(window_t *w, const char *html, int32_t len) {
    w->browser_line_count = 0;
    char cur_text[100]; int32_t cur_len = 0;
    char cur_link[64]; cur_link[0] = '\0';
    bool cur_heading = false;
    bool in_tag = false;
    bool in_skip_block = false; /* inside <script> or <style> */
    char tag_buf[16]; int32_t tag_len = 0;
    char attr_buf[400]; int32_t attr_len = 0;
    bool collecting_attrs = false;

    #define FLUSH_LINE() do { \
        if (cur_len > 0 || cur_link[0]) { \
            cur_text[cur_len] = '\0'; \
            browser_decode_entities(cur_text); \
            if (w->browser_line_count < 40) { \
                strncpy(w->browser_line_text[w->browser_line_count], cur_text, 99); \
                w->browser_line_text[w->browser_line_count][99] = '\0'; \
                strncpy(w->browser_line_link[w->browser_line_count], cur_link, 63); \
                w->browser_line_link[w->browser_line_count][63] = '\0'; \
                w->browser_line_heading[w->browser_line_count] = cur_heading; \
                w->browser_line_count++; \
            } \
        } \
        cur_len = 0; cur_link[0] = '\0'; cur_heading = false; \
    } while (0)

    for (int32_t i = 0; i < len; i++) {
        char c = html[i];

        if (in_skip_block) {
            if (c == '<' && i + 1 < len && html[i + 1] == '/') {
                if (strncmp(html + i, "</script", 8) == 0 || strncmp(html + i, "</style", 7) == 0 ||
                    strncmp(html + i, "</title", 7) == 0 || strncmp(html + i, "</head", 6) == 0 ||
                    strncmp(html + i, "</noscript", 10) == 0 || strncmp(html + i, "</select", 8) == 0 ||
                    strncmp(html + i, "</option", 8) == 0) {
                    in_skip_block = false;
                    while (i < len && html[i] != '>') i++;
                }
            }
            continue;
        }

        if (!in_tag && c == '<') {
            in_tag = true;
            tag_len = 0;
            attr_len = 0;
            collecting_attrs = false;
            continue;
        }

        if (in_tag) {
            if (c == '>') {
                in_tag = false;
                /* Strip a trailing '/' from self-closing tags like <br/> or <img .../> */
                if (tag_len > 0 && tag_buf[tag_len - 1] == '/') tag_len--;
                tag_buf[tag_len] = '\0';
                attr_buf[attr_len] = '\0';

                bool closing = (tag_buf[0] == '/');
                const char *name = closing ? tag_buf + 1 : tag_buf;

                if (strcmp(name, "br") == 0 || strcmp(name, "hr") == 0 ||
                    strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                    strcmp(name, "tr") == 0 || strcmp(name, "table") == 0 ||
                    strcmp(name, "thead") == 0 || strcmp(name, "tbody") == 0 || strcmp(name, "tfoot") == 0 ||
                    strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0 ||
                    strcmp(name, "dl") == 0 || strcmp(name, "dt") == 0 || strcmp(name, "dd") == 0 ||
                    strcmp(name, "header") == 0 || strcmp(name, "footer") == 0 ||
                    strcmp(name, "nav") == 0 || strcmp(name, "main") == 0 ||
                    strcmp(name, "section") == 0 || strcmp(name, "article") == 0 || strcmp(name, "aside") == 0 ||
                    strcmp(name, "blockquote") == 0 || strcmp(name, "pre") == 0 ||
                    strcmp(name, "form") == 0 || strcmp(name, "fieldset") == 0 ||
                    strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
                    FLUSH_LINE();
                    if (strcmp(name, "hr") == 0 && cur_link[0] == '\0') {
                        strcpy(cur_text, "----------------------------------------");
                        cur_len = (int32_t)strlen(cur_text);
                        FLUSH_LINE();
                    }
                } else if (strcmp(name, "li") == 0) {
                    FLUSH_LINE();
                    if (!closing) { cur_text[0] = '-'; cur_text[1] = ' '; cur_len = 2; }
                } else if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == '\0') {
                    if (closing) FLUSH_LINE();
                    else { FLUSH_LINE(); cur_heading = true; }
                } else if (strcmp(name, "a") == 0) {
                    if (closing) {
                        FLUSH_LINE();
                    } else {
                        const char *href = find_substr(attr_buf, attr_len, "href=");
                        cur_link[0] = '\0';
                        if (href) {
                            href += 5;
                            char quote = *href;
                            if (quote == '"' || quote == '\'') {
                                href++;
                                int32_t j = 0;
                                while (*href && *href != quote && j < 63) cur_link[j++] = *href++;
                                cur_link[j] = '\0';
                            }
                        }
                    }
                } else if (strcmp(name, "img") == 0) {
                    FLUSH_LINE();
                    const char *alt = find_substr(attr_buf, attr_len, "alt=");
                    char alt_text[40]; alt_text[0] = '\0';
                    if (alt) {
                        alt += 4;
                        char quote = *alt;
                        if (quote == '"' || quote == '\'') {
                            alt++;
                            int32_t j = 0;
                            while (*alt && *alt != quote && j < 39) alt_text[j++] = *alt++;
                            alt_text[j] = '\0';
                        }
                    }
                    strcpy(cur_text, "[IMAGE");
                    if (alt_text[0]) { strcat(cur_text, ": "); strncat(cur_text, alt_text, sizeof(cur_text) - strlen(cur_text) - 2); }
                    strcat(cur_text, "]");
                    cur_len = (int32_t)strlen(cur_text);
                    FLUSH_LINE();
                } else if (strcmp(name, "script") == 0 || strcmp(name, "style") == 0 ||
                           strcmp(name, "title") == 0 || strcmp(name, "head") == 0 ||
                           strcmp(name, "noscript") == 0 || strcmp(name, "select") == 0 ||
                           strcmp(name, "option") == 0) {
                    if (!closing) in_skip_block = true;
                    else FLUSH_LINE();
                }
                continue;
            }
            if (tag_len == 0 && (c == ' ' || c == '\t')) continue;
            if (c == ' ' && !collecting_attrs) collecting_attrs = true;
            if (!collecting_attrs && tag_len < (int32_t)sizeof(tag_buf) - 1) {
                tag_buf[tag_len++] = c;
            } else if (collecting_attrs && attr_len < (int32_t)sizeof(attr_buf) - 1) {
                attr_buf[attr_len++] = c;
            }
            continue;
        }

        /* Plain text content */
        if (c == '\r') continue;
        if (c == '\n' || c == '\t') c = ' ';
        if (c == ' ' && (cur_len == 0 || cur_text[cur_len - 1] == ' ')) continue;
        if (cur_len < (int32_t)sizeof(cur_text) - 1) cur_text[cur_len++] = c;
        if (cur_len >= (int32_t)sizeof(cur_text) - 1) FLUSH_LINE();
    }
    FLUSH_LINE();
    #undef FLUSH_LINE
}

static void browser_tick(window_t *w, bool *dirty) {
    if (w->browser_state == 1) { /* connecting */
        tcp_connection_t *conn = (tcp_connection_t*)w->browser_conn;
        if (tcp_is_established(conn)) {
            char host_only[64];
            strncpy(host_only, w->browser_url, sizeof(host_only) - 1);
            host_only[sizeof(host_only) - 1] = '\0';
            for (char *p = host_only; *p; p++) { if (*p == '/') { *p = '\0'; break; } }

            char req[160];
            strcpy(req, "GET ");
            strcat(req, w->browser_path);
            strcat(req, " HTTP/1.0\r\nHost: ");
            strcat(req, host_only);
            strcat(req, "\r\nConnection: close\r\n\r\n");
            tcp_send(conn, req, (uint16_t)strlen(req));
            w->browser_state = 2;
            *dirty = true;
        } else if (tcp_is_closed(conn)) {
            w->browser_state = 4;
            *dirty = true;
        }
    } else if (w->browser_state == 2) { /* loading */
        tcp_connection_t *conn = (tcp_connection_t*)w->browser_conn;
        char buf[512];
        int n = tcp_recv(conn, buf, sizeof(buf));
        if (n > 0) {
            int32_t space = (int32_t)sizeof(w->browser_response) - 1 - w->browser_response_len;
            int32_t copy = (n < space) ? n : space;
            if (copy > 0) {
                memcpy(w->browser_response + w->browser_response_len, buf, (size_t)copy);
                w->browser_response_len += copy;
                w->browser_response[w->browser_response_len] = '\0';
            }
            *dirty = true;
        }
        if (tcp_is_peer_closed(conn) || tcp_is_closed(conn)) {
            w->browser_state = 3;
            const char *split = find_substr(w->browser_response, w->browser_response_len, "\r\n\r\n");
            if (split) {
                const char *body = split + 4;
                int32_t body_len = w->browser_response_len - (int32_t)(split - w->browser_response) - 4;
                browser_parse_html(w, body, body_len);
            }
            *dirty = true;
        }
    }
}

#define BROWSER_BAR_H 22
#define BROWSER_GO_W  36
#define BROWSER_BACK_W 26
#define BROWSER_FWD_W  26
#define BROWSER_LINE_H 16

static void browser_back_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bw = BROWSER_BACK_W;
    *bh = BROWSER_BAR_H;
    *bx = w->x + 2;
    *by = w->y + TITLEBAR_HEIGHT + 2;
}

static void browser_fwd_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bw = BROWSER_FWD_W;
    *bh = BROWSER_BAR_H;
    *bx = w->x + 2 + BROWSER_BACK_W + 2;
    *by = w->y + TITLEBAR_HEIGHT + 2;
}

static void browser_go_button_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bw = BROWSER_GO_W;
    *bh = BROWSER_BAR_H;
    *bx = w->x + w->w - 2 - BROWSER_GO_W;
    *by = w->y + TITLEBAR_HEIGHT + 2;
}

static void browser_addr_bar_rect(const window_t *w, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    *bx = w->x + 4 + BROWSER_BACK_W + BROWSER_FWD_W + 4;
    *by = w->y + TITLEBAR_HEIGHT + 2;
    *bw = w->w - 6 - BROWSER_BACK_W - BROWSER_FWD_W - 4 - BROWSER_GO_W - 4;
    *bh = BROWSER_BAR_H;
}

/* Rect of rendered line `index` within the content area - shared by
   drawing and click hit-testing so they always agree. */
static void browser_line_rect(int32_t content_x, int32_t content_y, int index, int32_t *ly, int32_t *lh) {
    *ly = content_y + index * BROWSER_LINE_H;
    *lh = BROWSER_LINE_H;
    (void)content_x;
}

static void draw_browser(const window_t *w) {
    int32_t cx = w->x, cy = w->y + TITLEBAR_HEIGHT;
    int32_t cw = w->w, ch = w->h - TITLEBAR_HEIGHT;
    fb_fill_rect(cx, cy, cw, ch, COL_FACE);

    int32_t bbx, bby, bbw, bbh;
    browser_back_button_rect(w, &bbx, &bby, &bbw, &bbh);
    bool can_go_back = w->browser_history_count >= 2;
    draw_gradient_button(bbx, bby, bbw, bbh, !can_go_back);
    font_draw_string(bbx + (bbw - font_string_width("<", FONT_SCALE)) / 2,
                      bby + (bbh - font_char_height(FONT_SCALE)) / 2, "<", COL_TEXT_BLACK, FONT_SCALE);

    int32_t fbx, fby, fbw, fbh;
    browser_fwd_button_rect(w, &fbx, &fby, &fbw, &fbh);
    bool can_go_fwd = w->browser_forward_count >= 1;
    draw_gradient_button(fbx, fby, fbw, fbh, !can_go_fwd);
    font_draw_string(fbx + (fbw - font_string_width(">", FONT_SCALE)) / 2,
                      fby + (fbh - font_char_height(FONT_SCALE)) / 2, ">", COL_TEXT_BLACK, FONT_SCALE);

    int32_t abx, aby, abw, abh;
    browser_addr_bar_rect(w, &abx, &aby, &abw, &abh);
    fb_fill_rect(abx, aby, abw, abh, COL_WHITE);
    draw_bevel(abx, aby, abw, abh, false);
    const char *url_display = (w->browser_url_len > 0) ? w->browser_url : "TYPE ADDRESS (E.G. 93.184.216.34)";
    font_draw_string(abx + 4, aby + (abh - font_char_height(FONT_SCALE)) / 2, url_display, COL_TEXT_BLACK, FONT_SCALE);
    if (w->browser_editing_url) {
        int32_t tw = font_string_width(w->browser_url, FONT_SCALE);
        fb_fill_rect(abx + 4 + tw + 2, aby + 3, 2, abh - 6, COL_TEXT_BLACK);
    }

    int32_t gbx, gby, gbw, gbh;
    browser_go_button_rect(w, &gbx, &gby, &gbw, &gbh);
    draw_gradient_button(gbx, gby, gbw, gbh, false);
    int32_t gow = font_string_width("GO", FONT_SCALE);
    font_draw_string(gbx + (gbw - gow) / 2, gby + (gbh - font_char_height(FONT_SCALE)) / 2, "GO", COL_TEXT_BLACK, FONT_SCALE);

    int32_t content_y = cy + BROWSER_BAR_H + 4;
    int32_t content_h = ch - BROWSER_BAR_H - 6;
    fb_fill_rect(cx + 2, content_y, cw - 4, content_h, COL_WHITE);
    draw_bevel(cx + 2, content_y, cw - 4, content_h, false);

    const char *status = NULL;
    switch (w->browser_state) {
        case 1: status = "CONNECTING..."; break;
        case 2: status = "LOADING..."; break;
        case 4: status = "ERROR - COULD NOT LOAD PAGE"; break;
        default: break;
    }
    int32_t text_y = content_y + 4;
    if (status) {
        font_draw_string(cx + 8, text_y, status, COL_TEXT_BLACK, FONT_SCALE);
        text_y += font_char_height(FONT_SCALE) + 6;
    }

    if (w->browser_response_len > 0 && w->browser_state != 4) {
        if (w->browser_line_count == 0) {
            browser_draw_wrapped_text(cx + 8, text_y, cw - 16, content_y + content_h - 4,
                                       w->browser_response, w->browser_response_len, COL_TEXT_BLACK);
        } else {
            for (int32_t i = 0; i < w->browser_line_count; i++) {
                int32_t ly, lh;
                browser_line_rect(cx + 8, text_y, i, &ly, &lh);
                if (ly >= content_y + content_h - 4) break;
                bool is_link = w->browser_line_link[i][0] != '\0';
                int32_t scale = w->browser_line_heading[i] ? 2 : FONT_SCALE;
                uint32_t color = is_link ? fb_pack_color(0, 0, 200) : COL_TEXT_BLACK;
                font_draw_string(cx + 8, ly, w->browser_line_text[i], color, scale);
                if (is_link) {
                    int32_t tw = font_string_width(w->browser_line_text[i], scale);
                    fb_fill_rect(cx + 8, ly + font_char_height(scale), tw, 1, color);
                }
            }
        }
    } else if (w->browser_response_len > 0 && w->browser_state == 4) {
        browser_draw_wrapped_text(cx + 8, text_y, cw - 16, content_y + content_h - 4,
                                   w->browser_response, w->browser_response_len, COL_TEXT_BLACK);
    }
}

/* --- Context menu (right-click New/Open/Rename/Delete/Restore/Empty) --- */

#define CTX_MAX_ITEMS 6
typedef struct {
    bool open;
    int32_t x, y;
    char items[CTX_MAX_ITEMS][16];
    int item_count;
    int target_window;               /* -1 = desktop */
    char dir_path[NFS_MAX_PATH];      /* directory this menu operates within */
    char item_name[NFS_MAX_NAME];     /* "" = the "empty area" menu */
    bool item_is_dir;
    uint32_t item_size;
    bool is_recycle_bin_icon;         /* special-cased desktop Recycle Bin icon */
} ctx_menu_t;
static ctx_menu_t ctx_menu;

/* File clipboard - Copy/Paste between Explorer/Desktop locations (files only, v1). */
static bool clipboard_has_item = false;
static char clipboard_src_dir[NFS_MAX_PATH];
static char clipboard_name[NFS_MAX_NAME];

/* Properties dialog */
static bool props_dialog_open = false;

/* Right-click a Start menu item -> tiny popup offering "Set as Shortcut",
   which drops a small marker file on the desktop that launches the app
   when opened. */
static bool shortcut_menu_open = false;
static int32_t shortcut_menu_x = 0, shortcut_menu_y = 0;
static int shortcut_menu_item_index = -1;

#define SHORTCUT_MENU_W 150
#define SHORTCUT_MENU_H 20

static void draw_shortcut_menu(void) {
    if (!shortcut_menu_open) return;
    fb_fill_rect(shortcut_menu_x, shortcut_menu_y, SHORTCUT_MENU_W, SHORTCUT_MENU_H, COL_FACE);
    draw_bevel(shortcut_menu_x, shortcut_menu_y, SHORTCUT_MENU_W, SHORTCUT_MENU_H, true);
    font_draw_string(shortcut_menu_x + 6, shortcut_menu_y + (SHORTCUT_MENU_H - font_char_height(FONT_SCALE)) / 2,
                      "SET AS SHORTCUT", COL_TEXT_BLACK, FONT_SCALE);
}

/* Creates a tiny marker file on the Desktop that, when opened, launches
   the given Start menu app - our simple stand-in for a real "shortcut"
   file format. Content is just the menu_items[] index as text. */
static void create_shortcut(int menu_item_index) {
    if (menu_item_index < 0 || menu_item_index >= MENU_ITEM_COUNT) return;
    char name[NFS_MAX_NAME], full[NFS_MAX_PATH];
    path_make_unique("Desktop", menu_items[menu_item_index].label, ".lnk", name, sizeof(name));
    path_join("Desktop", name, full, sizeof(full));
    char content[4];
    content[0] = (char)('0' + menu_item_index);
    content[1] = '\0';
    nfs_write_path(full, content, 1);
    invalidate_all_explorer_caches();
}
static char props_name[NFS_MAX_NAME];
static bool props_is_dir;
static uint32_t props_size;

/* Drag-and-drop for desktop icons: drop onto Recycle Bin or a folder icon
   to move the file there. Distinguishes a click (open) from a drag (move)
   by whether the mouse moved past a small threshold before release. */
static bool dnd_candidate = false;
static bool dnd_active = false;
static char dnd_name[NFS_MAX_NAME];
static bool dnd_is_dir = false;
static int32_t dnd_start_x = 0, dnd_start_y = 0;
static int dnd_src_window = -1;      /* -1 = desktop, else index into windows[] (an Explorer window) */
static char dnd_src_dir[NFS_MAX_PATH]; /* folder the item is being dragged out of */

/* --- Inline rename (typing directly over an icon's name, like Explorer) --- */
static void start_rename(int window, const char *dir_path, const char *name) {
    renaming_active = true;
    renaming_window = window;
    strncpy(renaming_dir_path, dir_path, sizeof(renaming_dir_path) - 1);
    renaming_dir_path[sizeof(renaming_dir_path) - 1] = '\0';
    strncpy(renaming_old_name, name, sizeof(renaming_old_name) - 1);
    renaming_old_name[sizeof(renaming_old_name) - 1] = '\0';
    strncpy(renaming_buf, name, sizeof(renaming_buf) - 1);
    renaming_buf[sizeof(renaming_buf) - 1] = '\0';
    renaming_len = (int32_t)strlen(renaming_buf);
    force_redraw = true;
}

static void finish_rename(bool confirm) {
    if (confirm && renaming_buf[0] && strcmp(renaming_buf, renaming_old_name) != 0) {
        nfs_rename_path(renaming_dir_path, renaming_old_name, renaming_buf);
        invalidate_all_explorer_caches();
    }
    renaming_active = false;
    force_redraw = true;
}

/* Opens a file/folder - a folder navigates the given Explorer window (or a
   new one if window<0, e.g. from the desktop), a .txt file opens Notepad. */
static void open_item(const char *dir_path, const char *name, bool is_dir, int window) {
    char full[NFS_MAX_PATH];
    path_join(dir_path, name, full, sizeof(full));

    if (is_dir) {
        if (window >= 0 && windows[window].in_use && windows[window].app_type == APP_EXPLORER) {
            strncpy(windows[window].explorer_path, full, sizeof(windows[window].explorer_path) - 1);
            windows[window].explorer_path[sizeof(windows[window].explorer_path) - 1] = '\0';
        } else {
            wm_open_window_at(name, COL_WINDOW_CONTENT_DEFAULT, APP_EXPLORER, full);
        }
    } else {
        size_t len = strlen(name);
        bool is_txt = (len > 4 && strcmp(name + len - 4, ".txt") == 0);
        bool is_lnk = (len > 4 && strcmp(name + len - 4, ".lnk") == 0);
        bool is_bmp = (len > 4 && strcmp(name + len - 4, ".bmp") == 0);
        if (is_lnk) {
            char content[4];
            int n = nfs_read_path(full, content, sizeof(content) - 1);
            if (n > 0) {
                content[n] = '\0';
                int idx = content[0] - '0';
                if (idx >= 0 && idx < MENU_ITEM_COUNT && menu_items[idx].power_action == 0) {
                    wm_open_window(menu_items[idx].label, COL_WINDOW_CONTENT_DEFAULT, menu_items[idx].app_type);
                }
            }
        } else if (is_txt) {
            wm_open_window_at(name, COL_WINDOW_CONTENT_DEFAULT, APP_NOTEPAD, full);
        } else if (is_bmp) {
            wm_open_window_at(name, COL_WINDOW_CONTENT_DEFAULT, APP_PAINT, full);
        }
    }
    force_redraw = true;
}

static void recycle_bin_empty(void) {
    explorer_count = 0;
    nfs_list_path("Recycle Bin", explorer_collect);
    for (int i = 0; i < explorer_count; i++) {
        char full[NFS_MAX_PATH];
        path_join("Recycle Bin", explorer_names[i], full, sizeof(full));
        nfs_delete_path(full); /* non-empty subfolders are silently skipped - v1 limitation */
    }
    force_redraw = true;
}

static void ctx_menu_open_empty(int32_t x, int32_t y, int window, const char *dir_path) {
    ctx_menu.open = true;
    ctx_menu.x = x; ctx_menu.y = y;
    ctx_menu.target_window = window;
    strncpy(ctx_menu.dir_path, dir_path, sizeof(ctx_menu.dir_path) - 1);
    ctx_menu.dir_path[sizeof(ctx_menu.dir_path) - 1] = '\0';
    ctx_menu.item_name[0] = '\0';
    ctx_menu.is_recycle_bin_icon = false;
    int n = 0;
    strcpy(ctx_menu.items[n++], "NEW FOLDER");
    strcpy(ctx_menu.items[n++], "NEW TEXT FILE");
    if (clipboard_has_item) strcpy(ctx_menu.items[n++], "PASTE");
    ctx_menu.item_count = n;
}

static void ctx_menu_open_item(int32_t x, int32_t y, int window, const char *dir_path,
                                const char *name, bool is_dir, uint32_t size, bool is_recycle_bin_icon) {
    ctx_menu.open = true;
    ctx_menu.x = x; ctx_menu.y = y;
    ctx_menu.target_window = window;
    strncpy(ctx_menu.dir_path, dir_path, sizeof(ctx_menu.dir_path) - 1);
    ctx_menu.dir_path[sizeof(ctx_menu.dir_path) - 1] = '\0';
    strncpy(ctx_menu.item_name, name, sizeof(ctx_menu.item_name) - 1);
    ctx_menu.item_name[sizeof(ctx_menu.item_name) - 1] = '\0';
    ctx_menu.item_is_dir = is_dir;
    ctx_menu.item_size = size;
    ctx_menu.is_recycle_bin_icon = is_recycle_bin_icon;

    bool in_recycle_bin = (strcmp(dir_path, "Recycle Bin") == 0);
    size_t name_len = strlen(name);
    bool is_bmp = (!is_dir && name_len > 4 && strcmp(name + name_len - 4, ".bmp") == 0);

    int n = 0;
    if (is_recycle_bin_icon) {
        strcpy(ctx_menu.items[n++], "OPEN");
        strcpy(ctx_menu.items[n++], "EMPTY");
        strcpy(ctx_menu.items[n++], "RENAME");
    } else if (in_recycle_bin) {
        strcpy(ctx_menu.items[n++], "RESTORE");
        strcpy(ctx_menu.items[n++], "DELETE");
    } else {
        if (is_dir) strcpy(ctx_menu.items[n++], "OPEN");
        strcpy(ctx_menu.items[n++], "RENAME");
        strcpy(ctx_menu.items[n++], "DELETE");
        if (!is_dir && n < CTX_MAX_ITEMS) strcpy(ctx_menu.items[n++], "COPY");
        if (is_bmp && n < CTX_MAX_ITEMS) strcpy(ctx_menu.items[n++], "SET WALLPAPER");
        if (n < CTX_MAX_ITEMS) strcpy(ctx_menu.items[n++], "PROPERTIES");
    }
    ctx_menu.item_count = n;
}

static void ctx_menu_execute(int idx) {
    const char *action = ctx_menu.items[idx];

    if (strcmp(action, "NEW FOLDER") == 0) {
        char name[NFS_MAX_NAME], full[NFS_MAX_PATH];
        path_make_unique(ctx_menu.dir_path, "New Folder", "", name, sizeof(name));
        path_join(ctx_menu.dir_path, name, full, sizeof(full));
        nfs_mkdir_path(full);
    } else if (strcmp(action, "NEW TEXT FILE") == 0) {
        char name[NFS_MAX_NAME], full[NFS_MAX_PATH];
        path_make_unique(ctx_menu.dir_path, "New Text File", ".txt", name, sizeof(name));
        path_join(ctx_menu.dir_path, name, full, sizeof(full));
        nfs_write_path(full, "", 0);
    } else if (strcmp(action, "DELETE") == 0) {
        if (strcmp(ctx_menu.item_name, "SYSTEM.CFG") == 0) {
            /* protected system file - refuse silently, it shouldn't even be visible to select this on */
        } else if (strcmp(ctx_menu.dir_path, "Recycle Bin") == 0) {
            char full[NFS_MAX_PATH];
            path_join(ctx_menu.dir_path, ctx_menu.item_name, full, sizeof(full));
            nfs_delete_path(full);
        } else {
            nfs_move_path(ctx_menu.dir_path, ctx_menu.item_name, "Recycle Bin");
        }
    } else if (strcmp(action, "RESTORE") == 0) {
        nfs_move_path("Recycle Bin", ctx_menu.item_name, "Desktop");
    } else if (strcmp(action, "EMPTY") == 0) {
        recycle_bin_empty();
    } else if (strcmp(action, "RENAME") == 0) {
        start_rename(ctx_menu.target_window, ctx_menu.dir_path, ctx_menu.item_name);
    } else if (strcmp(action, "OPEN") == 0) {
        open_item(ctx_menu.dir_path, ctx_menu.item_name, ctx_menu.item_is_dir, ctx_menu.target_window);
    } else if (strcmp(action, "COPY") == 0) {
        strncpy(clipboard_src_dir, ctx_menu.dir_path, sizeof(clipboard_src_dir) - 1);
        clipboard_src_dir[sizeof(clipboard_src_dir) - 1] = '\0';
        strncpy(clipboard_name, ctx_menu.item_name, sizeof(clipboard_name) - 1);
        clipboard_name[sizeof(clipboard_name) - 1] = '\0';
        clipboard_has_item = true;
    } else if (strcmp(action, "PASTE") == 0 && clipboard_has_item) {
        char src_full[NFS_MAX_PATH];
        path_join(clipboard_src_dir, clipboard_name, src_full, sizeof(src_full));
        static uint8_t clip_buf[NFS_MAX_FILE_SIZE];
        int n = nfs_read_path(src_full, clip_buf, sizeof(clip_buf));
        if (n >= 0) {
            const char *dot = strrchr(clipboard_name, '.');
            char base[NFS_MAX_NAME], ext[16];
            if (dot) {
                size_t blen = (size_t)(dot - clipboard_name);
                if (blen >= sizeof(base)) blen = sizeof(base) - 1;
                memcpy(base, clipboard_name, blen); base[blen] = '\0';
                strncpy(ext, dot, sizeof(ext) - 1); ext[sizeof(ext) - 1] = '\0';
            } else {
                strncpy(base, clipboard_name, sizeof(base) - 1); base[sizeof(base) - 1] = '\0';
                ext[0] = '\0';
            }
            char unique[NFS_MAX_NAME], dst_full[NFS_MAX_PATH];
            path_make_unique(ctx_menu.dir_path, base, ext, unique, sizeof(unique));
            path_join(ctx_menu.dir_path, unique, dst_full, sizeof(dst_full));
            nfs_write_path(dst_full, clip_buf, (uint32_t)n);
        }
    } else if (strcmp(action, "PROPERTIES") == 0) {
        strncpy(props_name, ctx_menu.item_name, sizeof(props_name) - 1);
        props_name[sizeof(props_name) - 1] = '\0';
        props_is_dir = ctx_menu.item_is_dir;
        props_size = ctx_menu.item_size;
        props_dialog_open = true;
    } else if (strcmp(action, "SET WALLPAPER") == 0) {
        char full[NFS_MAX_PATH];
        path_join(ctx_menu.dir_path, ctx_menu.item_name, full, sizeof(full));
        wallpaper_set_from_file(full);
    }
    invalidate_all_explorer_caches();
    force_redraw = true;
}

#define CTX_ITEM_H 20
#define CTX_MENU_W 130

static void draw_context_menu(void) {
    if (!ctx_menu.open) return;
    int32_t h = ctx_menu.item_count * CTX_ITEM_H;
    fb_fill_rect(ctx_menu.x, ctx_menu.y, CTX_MENU_W, h, COL_FACE);
    draw_bevel(ctx_menu.x, ctx_menu.y, CTX_MENU_W, h, true);
    for (int i = 0; i < ctx_menu.item_count; i++) {
        int32_t iy = ctx_menu.y + i * CTX_ITEM_H;
        font_draw_string(ctx_menu.x + 6, iy + (CTX_ITEM_H - font_char_height(FONT_SCALE)) / 2,
                          ctx_menu.items[i], COL_TEXT_BLACK, FONT_SCALE);
    }
}

#define PROPS_DLG_W 220
#define PROPS_DLG_H 130

static void props_dialog_rect(int32_t *x, int32_t *y) {
    *x = (screen_w - PROPS_DLG_W) / 2;
    *y = (screen_h - PROPS_DLG_H) / 2;
}

static void props_close_button_rect(int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    int32_t dx, dy;
    props_dialog_rect(&dx, &dy);
    *bw = 60; *bh = 22;
    *bx = dx + PROPS_DLG_W - *bw - 10;
    *by = dy + PROPS_DLG_H - *bh - 10;
}

static void draw_props_dialog(void) {
    if (!props_dialog_open) return;
    int32_t dx, dy;
    props_dialog_rect(&dx, &dy);

    fb_fill_rect(dx, dy, PROPS_DLG_W, PROPS_DLG_H, COL_FACE);
    draw_bevel(dx, dy, PROPS_DLG_W, PROPS_DLG_H, true);
    fb_fill_rect_vgradient(dx, dy, PROPS_DLG_W, 22, 130, 190, 110, 35, 90, 40);
    font_draw_string(dx + 6, dy + 4, "PROPERTIES", COL_TITLEBAR_TEXT, FONT_SCALE);

    int32_t ty = dy + 30;
    char line[64];
    strcpy(line, "NAME: ");
    strncat(line, props_name, sizeof(line) - strlen(line) - 1);
    font_draw_string(dx + 10, ty, line, COL_TEXT_BLACK, FONT_SCALE);
    ty += 18;

    font_draw_string(dx + 10, ty, props_is_dir ? "TYPE: FOLDER" : "TYPE: FILE", COL_TEXT_BLACK, FONT_SCALE);
    ty += 18;

    if (!props_is_dir) {
        char sizebuf[32];
        strcpy(sizebuf, "SIZE: ");
        int len = (int)strlen(sizebuf);
        uint32_t sz = props_size;
        char digits[12]; int di = 0;
        if (sz == 0) digits[di++] = '0';
        while (sz > 0) { digits[di++] = (char)('0' + sz % 10); sz /= 10; }
        while (di > 0) sizebuf[len++] = digits[--di];
        strcpy(sizebuf + len, " BYTES");
        font_draw_string(dx + 10, ty, sizebuf, COL_TEXT_BLACK, FONT_SCALE);
    }

    int32_t bx, by, bw, bh;
    props_close_button_rect(&bx, &by, &bw, &bh);
    draw_gradient_button(bx, by, bw, bh, false);
    font_draw_string(bx + (bw - font_string_width("CLOSE", FONT_SCALE)) / 2,
                      by + (bh - font_char_height(FONT_SCALE)) / 2, "CLOSE", COL_TEXT_BLACK, FONT_SCALE);
}

static bool props_dialog_handle_click(int32_t mx, int32_t my) {
    if (!props_dialog_open) return false;
    props_dialog_open = false; /* any click (including CLOSE) dismisses it */
    (void)mx; (void)my;
    return true;
}

/* Returns true if the click was consumed by the menu (whether it hit an
   item or just closed the menu by clicking elsewhere). */
static bool ctx_menu_handle_click(int32_t mx, int32_t my) {
    if (!ctx_menu.open) return false;
    int32_t h = ctx_menu.item_count * CTX_ITEM_H;
    if (point_in_rect(mx, my, ctx_menu.x, ctx_menu.y, CTX_MENU_W, h)) {
        int idx = (int)((my - ctx_menu.y) / CTX_ITEM_H);
        if (idx >= 0 && idx < ctx_menu.item_count) ctx_menu_execute(idx);
    }
    ctx_menu.open = false;
    force_redraw = true;
    return true;
}

/* --- Desktop icons (My Computer + Recycle Bin + contents of the Desktop folder) --- */

#define DESKTOP_ICON_W 72
#define DESKTOP_ICON_H 70
#define DESKTOP_FIXED_COUNT 2
#define DESKTOP_MAX_DYN 16

static char desktop_names[DESKTOP_MAX_DYN][NFS_MAX_NAME];
static bool desktop_is_dir[DESKTOP_MAX_DYN];
static uint32_t desktop_sizes[DESKTOP_MAX_DYN];
static int  desktop_dyn_count = 0;

static int recycle_count_tmp = 0;
static void recycle_count_collect(const char *name, uint32_t size, bool is_dir) {
    (void)name; (void)size; (void)is_dir;
    recycle_count_tmp++;
}
static bool recycle_bin_empty_cached = true;
static bool recycle_bin_is_empty(void) {
    if (!recycle_bin_empty_cache_valid) {
        recycle_count_tmp = 0;
        nfs_list_path("Recycle Bin", recycle_count_collect);
        recycle_bin_empty_cached = (recycle_count_tmp == 0);
        recycle_bin_empty_cache_valid = true;
    }
    return recycle_bin_empty_cached;
}

static void desktop_collect(const char *name, uint32_t size, bool is_dir) {
    if (desktop_dyn_count < DESKTOP_MAX_DYN) {
        strncpy(desktop_names[desktop_dyn_count], name, NFS_MAX_NAME - 1);
        desktop_names[desktop_dyn_count][NFS_MAX_NAME - 1] = '\0';
        desktop_is_dir[desktop_dyn_count] = is_dir;
        desktop_sizes[desktop_dyn_count] = size;
        desktop_dyn_count++;
    }
}

static void desktop_icon_rect(int index, int32_t *ix, int32_t *iy) {
    int rows = (screen_h - TASKBAR_HEIGHT - 10) / DESKTOP_ICON_H;
    if (rows < 1) rows = 1;
    int col = index / rows;
    int row = index % rows;
    *ix = 10 + col * DESKTOP_ICON_W;
    *iy = 10 + row * DESKTOP_ICON_H;
}

static void draw_desktop_label(int32_t center_x, int32_t y, const char *text) {
    int32_t tw = font_string_width(text, FONT_SCALE);
    int32_t bx = center_x - tw / 2 - 2;
    /* Keep the whole label on-screen even if that means it's not
       perfectly centered - icons near the left/right edge with a long
       name (e.g. "MY COMPUTER", "RECYCLE BIN") were losing their first
       letter off the edge of the screen otherwise. */
    if (bx < 2) bx = 2;
    if (bx + tw + 4 > screen_w - 2) bx = screen_w - 2 - (tw + 4);
    fb_fill_rect(bx, y - 1, tw + 4, font_char_height(FONT_SCALE) + 2, fb_pack_color(0, 0, 0));
    font_draw_string(bx + 2, y, text, fb_pack_color(255, 255, 255), FONT_SCALE);
}

static void draw_desktop_icons(void) {
    int32_t ix, iy;

    desktop_icon_rect(0, &ix, &iy);
    if (renaming_active && renaming_window == -1 && strcmp(renaming_old_name, "MY COMPUTER") == 0) {
        icon_draw(ix, iy, ICON_DISK, 2);
        draw_desktop_label(ix + 16, iy + 36, renaming_buf);
    } else {
        icon_draw(ix, iy, ICON_DISK, 2);
        draw_desktop_label(ix + 16, iy + 36, "MY COMPUTER");
    }

    desktop_icon_rect(1, &ix, &iy);
    icon_draw(ix, iy, recycle_bin_is_empty() ? ICON_TRASH : ICON_TRASH_FULL, 2);
    if (renaming_active && renaming_window == -1 && strcmp(renaming_old_name, "Recycle Bin") == 0) {
        draw_desktop_label(ix + 16, iy + 36, renaming_buf);
    } else {
        draw_desktop_label(ix + 16, iy + 36, "RECYCLE BIN");
    }

    if (!desktop_cache_valid) {
        desktop_dyn_count = 0;
        nfs_list_path("Desktop", desktop_collect);
        desktop_cache_valid = true;
    }
    for (int i = 0; i < desktop_dyn_count; i++) {
        desktop_icon_rect(DESKTOP_FIXED_COUNT + i, &ix, &iy);
        icon_draw(ix, iy, desktop_is_dir[i] ? ICON_FOLDER : ICON_FILE, 2);
        const char *label = desktop_names[i];
        if (renaming_active && renaming_window == -1 && strcmp(renaming_old_name, desktop_names[i]) == 0) {
            label = renaming_buf;
        }
        char short_name[12];
        strncpy(short_name, label, 11);
        short_name[11] = '\0';
        draw_desktop_label(ix + 16, iy + 36, short_name);
    }
}

/* --- Terminal --- */

static void term_append(window_t *w, const char *text) {
    int32_t len = (int32_t)strlen(text);
    int32_t cap = (int32_t)sizeof(w->term_output) - 1;
    if (len > cap) { text += (len - cap); len = cap; } /* pathological: text alone longer than the buffer */

    int32_t avail = cap - w->term_output_len;
    if (len > avail) {
        int32_t need = len - avail;
        int32_t cut = 0;
        while (cut < w->term_output_len && need > 0) {
            cut++;
            need--;
            if (w->term_output[cut - 1] == '\n') break;
        }
        while (cut < w->term_output_len && w->term_output[cut - 1] != '\n') cut++;
        memmove(w->term_output, w->term_output + cut, (size_t)(w->term_output_len - cut));
        w->term_output_len -= cut;
    }

    avail = cap - w->term_output_len;
    int32_t copy = len < avail ? len : avail;
    memcpy(w->term_output + w->term_output_len, text, (size_t)copy);
    w->term_output_len += copy;
    w->term_output[w->term_output_len] = '\0';
}

static void term_append_line(window_t *w, const char *text) {
    term_append(w, text);
    term_append(w, "\n");
}

static void term_list_collect(const char *name, uint32_t size, bool is_dir) {
    if (strcmp(name, "SYSTEM.CFG") == 0) return; /* internal marker file, not meant to be seen/touched */
    if (explorer_count < EXPLORER_MAX_FILES) {
        strncpy(explorer_names[explorer_count], name, 31);
        explorer_names[explorer_count][31] = '\0';
        explorer_sizes[explorer_count] = size;
        explorer_is_dir[explorer_count] = is_dir;
        explorer_count++;
    }
}

/* Splits "cmd rest" into two null-terminated strings (trims leading spaces on `rest`). */
static void term_split(const char *line, char *cmd, size_t cmd_size, char *rest, size_t rest_size) {
    int32_t i = 0;
    while (line[i] && line[i] != ' ' && (size_t)i < cmd_size - 1) { cmd[i] = line[i]; i++; }
    cmd[i] = '\0';
    const char *r = line + i;
    while (*r == ' ') r++;
    strncpy(rest, r, rest_size - 1);
    rest[rest_size - 1] = '\0';
}

static void term_execute(window_t *w, const char *line) {
    char prompt_line[NFS_MAX_PATH + 8];
    strcpy(prompt_line, w->term_cwd[0] ? w->term_cwd : "/");
    strcat(prompt_line, "> ");
    strncat(prompt_line, line, sizeof(prompt_line) - strlen(prompt_line) - 1);
    term_append_line(w, prompt_line);

    if (line[0] == '\0') return;

    char cmd[16], rest[NFS_MAX_PATH];
    term_split(line, cmd, sizeof(cmd), rest, sizeof(rest));

    char full[NFS_MAX_PATH];
    if (rest[0]) path_join(w->term_cwd, rest, full, sizeof(full));

    if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "dir") == 0) {
        explorer_count = 0;
        nfs_list_path(rest[0] ? full : w->term_cwd, term_list_collect);
        if (explorer_count == 0) term_append_line(w, "(empty)");
        for (int i = 0; i < explorer_count; i++) {
            char line2[48];
            strcpy(line2, explorer_is_dir[i] ? "[DIR]  " : "       ");
            strncat(line2, explorer_names[i], sizeof(line2) - strlen(line2) - 1);
            term_append_line(w, line2);
        }
    } else if (strcmp(cmd, "cd") == 0) {
        if (rest[0] == '\0' || strcmp(rest, "/") == 0) {
            w->term_cwd[0] = '\0';
        } else if (strcmp(rest, "..") == 0) {
            char parent[NFS_MAX_PATH];
            path_parent(w->term_cwd, parent, sizeof(parent));
            strncpy(w->term_cwd, parent, sizeof(w->term_cwd) - 1);
        } else if (nfs_is_dir_path(full)) {
            strncpy(w->term_cwd, full, sizeof(w->term_cwd) - 1);
        } else {
            term_append_line(w, "no such directory");
        }
    } else if (strcmp(cmd, "pwd") == 0) {
        term_append_line(w, w->term_cwd[0] ? w->term_cwd : "/");
    } else if (strcmp(cmd, "cat") == 0 || strcmp(cmd, "type") == 0) {
        if (!rest[0]) { term_append_line(w, "usage: cat <file>"); }
        else {
            char buf[512];
            int n = nfs_read_path(full, buf, sizeof(buf) - 1);
            if (n < 0) term_append_line(w, "file not found");
            else { buf[n] = '\0'; term_append(w, buf); term_append(w, "\n"); }
        }
    } else if (strcmp(cmd, "mkdir") == 0) {
        if (!rest[0]) term_append_line(w, "usage: mkdir <name>");
        else term_append_line(w, nfs_mkdir_path(full) >= 0 ? "ok" : "error");
    } else if (strcmp(cmd, "touch") == 0) {
        if (!rest[0]) term_append_line(w, "usage: touch <name>");
        else term_append_line(w, nfs_write_path(full, "", 0) >= 0 ? "ok" : "error");
    } else if (strcmp(cmd, "rm") == 0 || strcmp(cmd, "del") == 0) {
        if (!rest[0]) term_append_line(w, "usage: rm <name>");
        else if (strcmp(rest, "SYSTEM.CFG") == 0) term_append_line(w, "cannot delete: protected system file");
        else term_append_line(w, nfs_delete_path(full) == 0 ? "ok" : "error (not empty, or not found)");
    } else if (strcmp(cmd, "echo") == 0) {
        term_append_line(w, rest);
    } else if (strcmp(cmd, "fps") == 0) {
        if (strcmp(rest, "on") == 0) { show_rps_counter = true; term_append_line(w, "fps counter on"); }
        else if (strcmp(rest, "off") == 0) { show_rps_counter = false; term_append_line(w, "fps counter off"); }
        else { show_rps_counter = !show_rps_counter; term_append_line(w, show_rps_counter ? "fps counter on" : "fps counter off"); }
    } else if (strcmp(cmd, "cls") == 0 || strcmp(cmd, "clear") == 0) {
        w->term_output_len = 0;
        w->term_output[0] = '\0';
    } else if (strcmp(cmd, "help") == 0) {
        term_append_line(w, "ls/dir cd pwd cat/type mkdir touch rm/del echo fps cls help");
    } else {
        term_append_line(w, "unknown command (try 'help')");
    }
    invalidate_all_explorer_caches();
}

#define TERM_LINE_H 12

static void draw_terminal(const window_t *w) {
    int32_t cx = w->x, cy = w->y + TITLEBAR_HEIGHT;
    int32_t cw = w->w, ch = w->h - TITLEBAR_HEIGHT;
    uint32_t bg = fb_pack_color(8, 8, 8);
    uint32_t fg = fb_pack_color(60, 220, 60);
    fb_fill_rect(cx, cy, cw, ch, bg);
    draw_bevel(cx, cy, cw, ch, false);

    int32_t input_h = 18;
    int32_t out_h = ch - input_h - 6;

    /* Render only the tail of the scrollback that fits the visible area */
    int32_t max_lines = out_h / TERM_LINE_H;
    int32_t total_lines = 1;
    for (int32_t i = 0; i < w->term_output_len; i++) if (w->term_output[i] == '\n') total_lines++;

    int32_t skip_lines = total_lines > max_lines ? total_lines - max_lines : 0;
    int32_t ty = cy + 4;
    int32_t line_no = 0;
    char linebuf[64]; int32_t ll = 0;
    for (int32_t i = 0; i <= w->term_output_len; i++) {
        char c = (i < w->term_output_len) ? w->term_output[i] : '\n';
        if (c == '\n') {
            if (line_no >= skip_lines) {
                linebuf[ll] = '\0';
                font_draw_string(cx + 4, ty, linebuf, fg, 1);
                ty += TERM_LINE_H;
            }
            line_no++;
            ll = 0;
        } else if (ll < (int32_t)sizeof(linebuf) - 1) {
            linebuf[ll++] = c;
        }
    }

    int32_t input_y = cy + ch - input_h - 2;
    fb_fill_rect(cx + 2, input_y, cw - 4, input_h, fb_pack_color(0, 0, 0));
    char prompt[NFS_MAX_PATH + 4];
    strcpy(prompt, w->term_cwd[0] ? w->term_cwd : "/");
    strcat(prompt, "> ");
    font_draw_string(cx + 4, input_y + 3, prompt, fg, 1);
    int32_t px = cx + 4 + font_string_width(prompt, 1) + 2;
    font_draw_string(px, input_y + 3, w->term_input, fg, 1);
    fb_fill_rect(px + font_string_width(w->term_input, 1) + 1, input_y + 2, 2, input_h - 4, fg);
}

/* Runs whatever .socks program a window's socks_path points at. Only one
   .socks program's code can be loaded (at the fixed SOCKS_LOAD_ADDR) at a
   time right now, so switching between two different .socks programs
   means reloading - fine for how few programs exist so far, but a real
   limitation worth revisiting if this grows into something with many
   programs open side by side. */
static socks_header_t *g_loaded_socks_header = NULL;
static char g_loaded_socks_path[NFS_MAX_PATH] = "";

static bool socks_ensure_loaded(const char *path) {
    if (strcmp(g_loaded_socks_path, path) == 0) return true;
    if (!socks_load(path, &g_loaded_socks_header)) {
        g_loaded_socks_path[0] = '\0';
        return false;
    }
    strncpy(g_loaded_socks_path, path, sizeof(g_loaded_socks_path) - 1);
    g_loaded_socks_path[sizeof(g_loaded_socks_path) - 1] = '\0';
    return true;
}

static void draw_socks_generic(const window_t *w) {
    int idx = (int)(w - windows);
    window_t *mw = &windows[idx];

    if (!socks_ensure_loaded(w->socks_path)) {
        fb_fill_rect(w->x, w->y + TITLEBAR_HEIGHT, w->w, w->h - TITLEBAR_HEIGHT, COL_WHITE);
        font_draw_string(w->x + 10, w->y + TITLEBAR_HEIGHT + 10,
                          "PROGRAM FILE MISSING OR CORRUPT", COL_TEXT_BLACK, FONT_SCALE);
        return;
    }

    mw->socks_ctx.x = w->x;
    mw->socks_ctx.y = w->y + TITLEBAR_HEIGHT;
    mw->socks_ctx.w = w->w;
    mw->socks_ctx.h = w->h - TITLEBAR_HEIGHT;
    strncpy(mw->socks_ctx.path, w->notepad_path, sizeof(mw->socks_ctx.path) - 1);
    mw->socks_ctx.path[sizeof(mw->socks_ctx.path) - 1] = '\0';

    if (!mw->socks_ready) {
        socks_call_init(g_loaded_socks_header, &mw->socks_ctx);
        mw->socks_ready = true;
    }

    socks_call_draw(g_loaded_socks_header, &mw->socks_ctx);
}

static void draw_app_content(const window_t *w) {
    switch (w->app_type) {
        case APP_NOTEPAD:    draw_socks_generic(w); break;
        case APP_SOCKS:      draw_socks_generic(w); break;
        case APP_CALCULATOR: draw_calculator(w); break;
        case APP_PAINT:      draw_paint(w); break;
        case APP_EXPLORER:   draw_explorer(w); break;
        case APP_BROWSER:    draw_browser(w); break;
        case APP_TERMINAL:   draw_terminal(w); break;
        default:
            fb_fill_rect(w->x, w->y + TITLEBAR_HEIGHT, w->w, w->h - TITLEBAR_HEIGHT, w->content_color);
            break;
    }
}

#define RESIZE_HANDLE_SIZE 14

static void resize_handle_rect(const window_t *w, int32_t *rx, int32_t *ry, int32_t *rw, int32_t *rh) {
    *rw = RESIZE_HANDLE_SIZE;
    *rh = RESIZE_HANDLE_SIZE;
    *rx = w->x + w->w - RESIZE_HANDLE_SIZE;
    *ry = w->y + w->h - RESIZE_HANDLE_SIZE;
}

static void draw_resize_handle(const window_t *w) {
    int32_t rx, ry, rw, rh;
    resize_handle_rect(w, &rx, &ry, &rw, &rh);
    for (int32_t i = 2; i < rw - 1; i += 3) {
        fb_fill_rect(rx + i, ry + rh - 2, 2, 2, COL_FACE_SHADOW);
        fb_fill_rect(rx + rw - 2, ry + i, 2, 2, COL_FACE_SHADOW);
    }
    for (int32_t i = 4; i < rw - 1; i += 3) {
        int32_t j = rw - i;
        fb_fill_rect(rx + i, ry + j, 2, 2, COL_FACE_SHADOW);
    }
}

static void draw_window(const window_t *w) {
    fb_fill_rect(w->x - 2, w->y - 2, w->w + 4, w->h + 4, COL_FACE_DARKSHADOW);
    draw_bevel(w->x - 1, w->y - 1, w->w + 2, w->h + 2, true);

    fb_fill_rect_vgradient(w->x, w->y, w->w, TITLEBAR_HEIGHT,
                           130, 190, 110,   /* top: light glossy green */
                           35, 90, 40);     /* bottom: dark green */
    font_draw_string(w->x + 6, w->y + (TITLEBAR_HEIGHT - font_char_height(FONT_SCALE)) / 2,
                      w->title, COL_TITLEBAR_TEXT, FONT_SCALE);

    int32_t mbx, mby;
    minimize_button_rect(w, &mbx, &mby);
    draw_gradient_button_rgb2(mbx, mby, BTN_SIZE, BTN_SIZE, false, fb_pack_color(30, 70, 160),
                               140, 190, 240, 30, 70, 160);
    fb_fill_rect(mbx + 4, mby + BTN_SIZE - 6, BTN_SIZE - 8, 2, COL_TEXT_BLACK);

    int32_t xbx, xby;
    maximize_button_rect(w, &xbx, &xby);
    draw_gradient_button_rgb2(xbx, xby, BTN_SIZE, BTN_SIZE, false, fb_pack_color(30, 70, 160),
                               140, 190, 240, 30, 70, 160);
    if (w->maximized) {
        fb_fill_rect(xbx + 4, xby + 5, BTN_SIZE - 9, BTN_SIZE - 9, COL_FACE);
        draw_bevel(xbx + 4, xby + 5, BTN_SIZE - 9, BTN_SIZE - 9, true);
        fb_fill_rect(xbx + 3, xby + 4, BTN_SIZE - 9, BTN_SIZE - 9, COL_FACE);
        draw_bevel(xbx + 3, xby + 4, BTN_SIZE - 9, BTN_SIZE - 9, true);
    } else {
        fb_fill_rect(xbx + 3, xby + 3, BTN_SIZE - 6, BTN_SIZE - 6, COL_FACE);
        draw_bevel(xbx + 3, xby + 3, BTN_SIZE - 6, BTN_SIZE - 6, true);
    }

    int32_t cbx, cby;
    close_button_rect(w, &cbx, &cby);
    draw_gradient_button_rgb2(cbx, cby, BTN_SIZE, BTN_SIZE, false, fb_pack_color(160, 25, 20),
                               240, 130, 120, 160, 25, 20);
    for (int i = 3; i < BTN_SIZE - 3; i++) {
        fb_put_pixel(cbx + i, cby + i, COL_TEXT_BLACK);
        fb_put_pixel(cbx + i + 1, cby + i, COL_TEXT_BLACK);
        fb_put_pixel(cbx + (BTN_SIZE - 1 - i), cby + i, COL_TEXT_BLACK);
        fb_put_pixel(cbx + (BTN_SIZE - 1 - i) - 1, cby + i, COL_TEXT_BLACK);
    }

    draw_app_content(w);
    draw_resize_handle(w);
}

static bool taskbar_button_rect(int window_index, int32_t *bx, int32_t *by, int32_t *bw, int32_t *bh) {
    if (!windows[window_index].in_use) return false;
    int32_t slot = 0;
    for (int i = 0; i < window_index; i++) {
        if (windows[i].in_use) slot++;
    }
    *bx = START_BTN_WIDTH + 6 + TASKBAR_BTN_GAP + slot * (TASKBAR_BTN_WIDTH + TASKBAR_BTN_GAP);
    *by = screen_h - TASKBAR_HEIGHT + 3;
    *bw = TASKBAR_BTN_WIDTH;
    *bh = TASKBAR_HEIGHT - 6;
    return true;
}

/* Small network status glyph: green ascending bars if the NIC came up,
   a red X if not. Drawn directly with primitives (no icon table entry). */
static void draw_network_icon(int32_t x, int32_t y) {
    draw_gradient_button(x, y, 20, 20, true);
    if (rtl8139_is_ready() || pcnet_is_ready()) {
        fb_fill_rect(x + 4, y + 12, 3, 5, COL_GREEN);
        fb_fill_rect(x + 8, y + 8, 3, 9, COL_GREEN);
        fb_fill_rect(x + 12, y + 4, 3, 13, COL_GREEN);
    } else {
        for (int i = 3; i < 16; i++) {
            fb_put_pixel(x + i, y + i, COL_RED);
            fb_put_pixel(x + i + 1, y + i, COL_RED);
            fb_put_pixel(x + (19 - i), y + i, COL_RED);
            fb_put_pixel(x + (19 - i) - 1, y + i, COL_RED);
        }
    }
}

static void draw_taskbar(void) {
    int32_t ty = screen_h - TASKBAR_HEIGHT;
    fb_fill_rect_vgradient(0, ty, screen_w, TASKBAR_HEIGHT,
                           130, 190, 110,   /* top: light glossy green */
                           35, 90, 40);     /* bottom: dark green */
    fb_fill_rect(0, ty, screen_w, 1, COL_FACE_LIGHT);

    draw_gradient_button(1, ty + 2, START_BTN_WIDTH, TASKBAR_HEIGHT - 4, start_menu_open);
    int32_t tw = font_string_width("START", FONT_SCALE);
    font_draw_string(1 + (START_BTN_WIDTH - tw) / 2,
                      ty + 2 + ((TASKBAR_HEIGHT - 4) - font_char_height(FONT_SCALE)) / 2,
                      "START", COL_TEXT_BLACK, FONT_SCALE);

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (!windows[i].in_use) continue;
        int32_t bx, by, bw, bh;
        taskbar_button_rect(i, &bx, &by, &bw, &bh);
        bool pressed_look = !windows[i].minimized;
        draw_gradient_button(bx, by, bw, bh, pressed_look);
        font_draw_string(bx + 6, by + (bh - font_char_height(FONT_SCALE)) / 2,
                          windows[i].title, COL_TEXT_BLACK, FONT_SCALE);
    }

    /* Clock + network icon, right-aligned */
    int32_t rps_reserved_w = 0;
    if (show_rps_counter) {
        char rps_text[16];
        { int n = last_rps; int ri = 0;
          rps_text[ri++]='R'; rps_text[ri++]=':';
          char digits[6]; int di=0;
          if (n==0) digits[di++]='0';
          while (n>0) { digits[di++]=(char)('0'+n%10); n/=10; }
          while (di>0) rps_text[ri++]=digits[--di];
          rps_text[ri]='\0';
        }
        int32_t rpsw = font_string_width(rps_text, FONT_SCALE);
        int32_t rps_panel_w = rpsw + 12;
        int32_t rps_panel_x = screen_w - rps_panel_w - 4;
        draw_gradient_button(rps_panel_x, ty + 4, rps_panel_w, TASKBAR_HEIGHT - 8, true);
        font_draw_string(rps_panel_x + 6, ty + 4 + ((TASKBAR_HEIGHT - 8) - font_char_height(FONT_SCALE)) / 2,
                          rps_text, COL_TEXT_BLACK, FONT_SCALE);
        rps_reserved_w = rps_panel_w + 4;
    }

    int32_t cw = font_string_width(clock_text, FONT_SCALE);
    int32_t clock_panel_w = cw + 16;
    int32_t clock_panel_x = screen_w - rps_reserved_w - clock_panel_w - 4;
    draw_gradient_button(clock_panel_x, ty + 4, clock_panel_w, TASKBAR_HEIGHT - 8, true);
    font_draw_string(clock_panel_x + 8, ty + 4 + ((TASKBAR_HEIGHT - 8) - font_char_height(FONT_SCALE)) / 2,
                      clock_text, COL_TEXT_BLACK, FONT_SCALE);

    draw_network_icon(clock_panel_x - 24, ty + 5);
}

static void start_menu_rect(int32_t *mx, int32_t *my, int32_t *mw, int32_t *mh) {
    *mw = MENU_WIDTH;
    *mh = (discovered_count + MENU_ITEM_COUNT) * MENU_ITEM_HEIGHT;
    *mx = 0;
    *my = screen_h - TASKBAR_HEIGHT - *mh;
}

static void menu_item_rect(int index, int32_t *ix, int32_t *iy, int32_t *iw, int32_t *ih) {
    int32_t mx, my, mw, mh;
    start_menu_rect(&mx, &my, &mw, &mh);
    *ix = mx;
    *iy = my + (discovered_count + index) * MENU_ITEM_HEIGHT;
    *iw = mw;
    *ih = MENU_ITEM_HEIGHT;
}

static void draw_start_menu(void) {
    int32_t mx, my, mw, mh;
    start_menu_rect(&mx, &my, &mw, &mh);
    fb_fill_rect(mx, my, mw, mh, COL_FACE);
    draw_bevel(mx, my, mw, mh, true);

    for (int i = 0; i < discovered_count; i++) {
        int32_t iy = my + i * MENU_ITEM_HEIGHT;
        if (discovered_programs[i].has_icon) {
            uint32_t small[16 * 16];
            for (int sy = 0; sy < 16; sy++) {
                for (int sx = 0; sx < 16; sx++) {
                    small[sy * 16 + sx] = discovered_programs[i].icon_pixels[(sy * 2) * 32 + (sx * 2)];
                }
            }
            fb_blit_packed(mx + 6, iy + (MENU_ITEM_HEIGHT - 16) / 2, 16, 16, small, 16);
        } else {
            icon_draw(mx + 6, iy + (MENU_ITEM_HEIGHT - 16) / 2, ICON_FILE, 1);
        }
        font_draw_string(mx + 28, iy + (MENU_ITEM_HEIGHT - font_char_height(FONT_SCALE)) / 2,
                          discovered_programs[i].name, COL_TEXT_BLACK, FONT_SCALE);
    }
    if (discovered_count > 0) {
        int32_t sep_y = my + discovered_count * MENU_ITEM_HEIGHT;
        fb_fill_rect(mx + 2, sep_y, mw - 4, 1, COL_FACE_SHADOW);
        fb_fill_rect(mx + 2, sep_y + 1, mw - 4, 1, COL_FACE_LIGHT);
    }

    for (int i = 0; i < MENU_ITEM_COUNT; i++) {
        int32_t ix, iy, iw, ih;
        menu_item_rect(i, &ix, &iy, &iw, &ih);

        if (menu_items[i].power_action != 0 && i > 0 && menu_items[i - 1].power_action == 0) {
            fb_fill_rect(ix + 2, iy, iw - 4, 1, COL_FACE_SHADOW);
            fb_fill_rect(ix + 2, iy + 1, iw - 4, 1, COL_FACE_LIGHT);
        }

        if (menu_items[i].has_icon) {
            icon_draw(ix + 6, iy + (ih - 16) / 2, menu_items[i].icon, 1);
        }
        font_draw_string(ix + 28, iy + (ih - font_char_height(FONT_SCALE)) / 2,
                          menu_items[i].label, COL_TEXT_BLACK, FONT_SCALE);
    }
}

static void log_ticks(const char *label, uint64_t delta) {
    char b[48]; int bi = 0;
    while (*label) b[bi++] = *label++;
    b[bi++] = '=';
    char d[8]; int di = 0;
    uint64_t n = delta;
    if (n == 0) d[di++] = '0';
    while (n > 0) { d[di++] = (char)('0' + n % 10); n /= 10; }
    while (di > 0) b[bi++] = d[--di];
    b[bi++] = '\n'; b[bi] = 0;
    serial_write(b);
}

static void redraw_all(void) {
    static int dbg_counter = 0;
    bool do_log = (++dbg_counter % 30 == 0); /* don't flood the log */
    uint64_t t0 = timer_ticks();

    wallpaper_draw();
    uint64_t t1 = timer_ticks();
    draw_desktop_icons();
    uint64_t t2 = timer_ticks();

    {
        const char *watermark = "ZEROS CODENAME NUGGET";
        int32_t ww = font_string_width(watermark, 1);
        font_draw_string(screen_w - ww - 8, screen_h - TASKBAR_HEIGHT - font_char_height(1) - 6,
                          watermark, fb_pack_color(255, 255, 255), 1);
    }

    for (int i = 0; i < z_count; i++) {
        int idx = z_order[i];
        if (windows[idx].in_use && !windows[idx].minimized) {
            if (windows[idx].app_type == APP_EXPLORER) explorer_refresh_cache_if_needed(&windows[idx]);
            draw_window(&windows[idx]);
        }
    }
    uint64_t t3 = timer_ticks();
    draw_taskbar();
    if (start_menu_open) draw_start_menu();
    draw_context_menu();
    draw_props_dialog();
    draw_shortcut_menu();
    uint64_t t4 = timer_ticks();

    if (do_log) {
        log_ticks("[perf] wallpaper", t1 - t0);
        log_ticks("[perf] icons", t2 - t1);
        log_ticks("[perf] windows", t3 - t2);
        log_ticks("[perf] taskbar", t4 - t3);
    }
}

/* Same as redraw_all, but only touches the rows in [y0,y1) - used while
   dragging a window, since the wallpaper and every other window are
   unchanged and re-drawing them anyway (even if the final copy to video
   memory was already limited to this range) still cost real time filling
   the back buffer for no reason. Only windows overlapping this band are
   redrawn at all. */
static void redraw_partial(int32_t y0, int32_t y1) {
    wallpaper_draw_rows(y0, y1);

    /* Desktop icons are cheap now that they're cached, and skipping them
       precisely would need per-icon overlap checks for little benefit -
       just draw them if the dirty band is anywhere in the icon column. */
    if (y0 < screen_h - TASKBAR_HEIGHT) draw_desktop_icons();

    for (int i = 0; i < z_count; i++) {
        int idx = z_order[i];
        window_t *win = &windows[idx];
        if (!win->in_use || win->minimized) continue;
        int32_t wtop = win->y - 8, wbot = win->y + win->h + 8; /* margin for shadow/bevel */
        if (wbot < y0 || wtop >= y1) continue; /* doesn't overlap the dirty band at all */
        if (win->app_type == APP_EXPLORER) explorer_refresh_cache_if_needed(win);
        draw_window(win);
    }

    if (y1 > screen_h - TASKBAR_HEIGHT) {
        draw_taskbar();
        if (start_menu_open) draw_start_menu();
    }
    draw_context_menu();
    draw_props_dialog();
}

static void update_clock(bool *dirty) {
    static uint64_t last_rtc_check = 0;
    uint64_t now = timer_ticks();
    /* The clock only needs checking roughly once a second - reading it on
       every single call (which can happen very often during a fast mouse
       drag) meant doing several CMOS I/O port accesses far more often than
       needed. Each of those is slow on real hardware and can trigger a
       full VM exit under virtualization, making this a surprisingly real
       cost when it ran hundreds of times a second for no benefit. */
    if (now - last_rtc_check < 100) return;
    last_rtc_check = now;

    uint8_t h, m, s;
    rtc_read_time(&h, &m, &s);
    if (h != last_hour || m != last_minute) {
        last_hour = h;
        last_minute = m;
        clock_text[0] = (char)('0' + h / 10);
        clock_text[1] = (char)('0' + h % 10);
        clock_text[2] = ':';
        clock_text[3] = (char)('0' + m / 10);
        clock_text[4] = (char)('0' + m % 10);
        clock_text[5] = '\0';
        *dirty = true;
    }
}

static void do_shutdown(void) {
    fb_clear(fb_pack_color(0, 0, 0));
    const char *msg = "IT IS NOW SAFE TO TURN OFF YOUR COMPUTER";
    int32_t tw = font_string_width(msg, 2);
    font_draw_string(((int32_t)fb_width() - tw) / 2, (int32_t)fb_height() / 2,
                      msg, fb_pack_color(255, 255, 255), 2);
    fb_present();

    /* Try the well-known "quick shutdown" ACPI PM ports that QEMU, Bochs,
       and VirtualBox's virtual chipsets respond to - this actually powers
       the machine off in an emulator instead of just halting forever. On
       real hardware without one of these specific virtual chipsets, none
       of these do anything, and we fall through to the safe old-school
       halt-and-wait-for-the-user-to-flip-the-switch behavior below. */
    outw(0x604, 0x2000);   /* QEMU (current versions) */
    outw(0xB004, 0x2000);  /* Bochs / older QEMU */
    outw(0x4004, 0x3400);  /* VirtualBox */

    for (;;) { __asm__ volatile ("cli; hlt"); }
}

static void do_restart(void) {
    /* Classic keyboard-controller reset trick */
    int timeout = 100000;
    while ((inb(0x64) & 0x02) && timeout--) { }
    outb(0x64, 0xFE);
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

void wm_handle_key(char c) {
    if (renaming_active) {
        if (c == '\n') {
            finish_rename(true);
        } else if (c == '\b') {
            if (renaming_len > 0) renaming_len--;
            renaming_buf[renaming_len] = '\0';
        } else if (renaming_len < (int32_t)sizeof(renaming_buf) - 1) {
            renaming_buf[renaming_len++] = c;
            renaming_buf[renaming_len] = '\0';
        }
        force_redraw = true;
        return;
    }

    if (z_count == 0) return;
    int idx = z_order[z_count - 1];
    window_t *w = &windows[idx];
    if (!w->in_use || w->minimized) return;

    if (w->app_type == APP_NOTEPAD && w->notepad_saving_as) {
        if (c == '\n') {
            if (w->notepad_saveas_len > 0) {
                nfs_write_path(w->notepad_saveas_buf, w->text, (uint32_t)w->text_len);
                invalidate_all_explorer_caches();
                strncpy(w->notepad_path, w->notepad_saveas_buf, sizeof(w->notepad_path) - 1);
                w->notepad_path[sizeof(w->notepad_path) - 1] = '\0';
            }
            w->notepad_saving_as = false;
        } else if (c == '\b') {
            if (w->notepad_saveas_len > 0) w->notepad_saveas_len--;
            w->notepad_saveas_buf[w->notepad_saveas_len] = '\0';
        } else if (w->notepad_saveas_len < (int32_t)sizeof(w->notepad_saveas_buf) - 1) {
            w->notepad_saveas_buf[w->notepad_saveas_len++] = c;
            w->notepad_saveas_buf[w->notepad_saveas_len] = '\0';
        }
        force_redraw = true;
    } else if (w->app_type == APP_NOTEPAD || w->app_type == APP_SOCKS) {
        if (socks_ensure_loaded(w->socks_path)) {
            socks_call_key(g_loaded_socks_header, &w->socks_ctx, c);
        }
        force_redraw = true;
    } else if (w->app_type == APP_PAINT && w->paint_saving_as) {
        if (c == '\n') {
            if (w->paint_saveas_len > 0 && w->paint_canvas) {
                static uint8_t bmp_key_save_buf[NFS_MAX_FILE_SIZE];
                int32_t sz = bmp_encode(w->paint_canvas, w->paint_canvas_w, w->paint_canvas_h, bmp_key_save_buf, sizeof(bmp_key_save_buf));
                if (sz > 0) {
                    nfs_write_path(w->paint_saveas_buf, bmp_key_save_buf, (uint32_t)sz);
                    invalidate_all_explorer_caches();
                    strncpy(w->paint_path, w->paint_saveas_buf, sizeof(w->paint_path) - 1);
                    w->paint_path[sizeof(w->paint_path) - 1] = '\0';
                }
            }
            w->paint_saving_as = false;
        } else if (c == '\b') {
            if (w->paint_saveas_len > 0) w->paint_saveas_len--;
            w->paint_saveas_buf[w->paint_saveas_len] = '\0';
        } else if (w->paint_saveas_len < (int32_t)sizeof(w->paint_saveas_buf) - 1) {
            w->paint_saveas_buf[w->paint_saveas_len++] = c;
            w->paint_saveas_buf[w->paint_saveas_len] = '\0';
        }
        force_redraw = true;
    } else if (w->app_type == APP_PAINT && w->paint_text_active) {
        if (c == '\n') {
            w->paint_text_active = false;
        } else if (c == '\b') {
            w->paint_text_x -= 6;
            if (w->paint_text_x < 0) w->paint_text_x = 0;
        } else if (w->paint_canvas) {
            paint_draw_char_to_canvas(w->paint_canvas, w->paint_canvas_w, w->paint_canvas_h,
                                       w->paint_text_x, w->paint_text_y, c, w->paint_color);
            w->paint_text_x += 6;
        }
        force_redraw = true;
    } else if (w->app_type == APP_CALCULATOR) {
        char label[2] = { c, '\0' };
        if ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == '*' || c == '/' || c == '=') {
            calc_handle_button(w, label);
        } else if (c == '\n') {
            calc_handle_button(w, "=");
        } else if (c == '\b' || c == 'c' || c == 'C') {
            calc_handle_button(w, "C");
        }
        force_redraw = true;
    } else if (w->app_type == APP_BROWSER && w->browser_editing_url) {
        if (c == '\n') {
            w->browser_editing_url = false;
            browser_navigate(w);
        } else if (c == '\b') {
            if (w->browser_url_len > 0) w->browser_url_len--;
            w->browser_url[w->browser_url_len] = '\0';
        } else if (w->browser_url_len < (int32_t)sizeof(w->browser_url) - 1) {
            w->browser_url[w->browser_url_len++] = c;
            w->browser_url[w->browser_url_len] = '\0';
        }
        force_redraw = true;
    } else if (w->app_type == APP_TERMINAL) {
        if (c == '\n') {
            term_execute(w, w->term_input);
            w->term_input[0] = '\0';
            w->term_input_len = 0;
        } else if (c == '\b') {
            if (w->term_input_len > 0) w->term_input_len--;
            w->term_input[w->term_input_len] = '\0';
        } else if (w->term_input_len < (int32_t)sizeof(w->term_input) - 1) {
            w->term_input[w->term_input_len++] = c;
            w->term_input[w->term_input_len] = '\0';
        }
        force_redraw = true;
    }
}

void wm_update(int32_t mouse_x, int32_t mouse_y, bool mouse_left_down, bool mouse_right_down) {
    bool dirty = force_redraw;
    force_redraw = false;

    update_clock(&dirty);

    bool click_down = mouse_left_down && !prev_mouse_left;
    bool click_up = !mouse_left_down && prev_mouse_left;
    bool click_down_right = mouse_right_down && !prev_mouse_right;

    /* A left-click anywhere while renaming just confirms it (clicking away
       to end an inline rename is standard Explorer/desktop behavior). */
    if (click_down && renaming_active) {
        finish_rename(true);
    }

    /* If the context menu is open, the next left-click is entirely about
       the menu (choose an item, or click elsewhere to dismiss it) - it
       doesn't fall through to anything else this frame. */
    if (click_down && ctx_menu.open) {
        ctx_menu_handle_click(mouse_x, mouse_y);
        prev_mouse_left = mouse_left_down;
        prev_mouse_right = mouse_right_down;
        if (dirty) { redraw_all(); cursor_reset(mouse_x, mouse_y); fb_present(); }
        return;
    }

    if (click_down && props_dialog_open) {
        props_dialog_handle_click(mouse_x, mouse_y);
        prev_mouse_left = mouse_left_down;
        prev_mouse_right = mouse_right_down;
        redraw_all();
        cursor_reset(mouse_x, mouse_y);
        fb_present();
        return;
    }

    if (click_down && shortcut_menu_open) {
        if (point_in_rect(mouse_x, mouse_y, shortcut_menu_x, shortcut_menu_y, SHORTCUT_MENU_W, SHORTCUT_MENU_H)) {
            create_shortcut(shortcut_menu_item_index);
        }
        shortcut_menu_open = false;
        prev_mouse_left = mouse_left_down;
        prev_mouse_right = mouse_right_down;
        redraw_all();
        cursor_reset(mouse_x, mouse_y);
        fb_present();
        return;
    }

    if (click_down_right) {
        bool handled = false;

        if (start_menu_open) {
            int32_t mx, my, mw, mh;
            start_menu_rect(&mx, &my, &mw, &mh);
            if (point_in_rect(mouse_x, mouse_y, mx, my, mw, mh)) {
                for (int i = 0; i < MENU_ITEM_COUNT; i++) {
                    if (menu_items[i].power_action != 0) continue; /* no shortcuts for shutdown/restart */
                    int32_t ix, iy, iw, ih;
                    menu_item_rect(i, &ix, &iy, &iw, &ih);
                    if (point_in_rect(mouse_x, mouse_y, ix, iy, iw, ih)) {
                        shortcut_menu_open = true;
                        shortcut_menu_item_index = i;
                        shortcut_menu_x = mouse_x;
                        shortcut_menu_y = mouse_y;
                        handled = true;
                        break;
                    }
                }
            }
            start_menu_open = false;
            dirty = true;
        }

        for (int zi = z_count - 1; zi >= 0 && !handled; zi--) {
            int i = z_order[zi];
            if (!windows[i].in_use || windows[i].minimized) continue;
            if (!point_in_rect(mouse_x, mouse_y, windows[i].x, windows[i].y, windows[i].w, windows[i].h)) continue;

            handled = true;
            z_bring_to_front(i);
            if (windows[i].app_type == APP_EXPLORER &&
                mouse_y >= windows[i].y + TITLEBAR_HEIGHT + 26) {
                int32_t addr_h = 22;
                int32_t list_y = (windows[i].y + TITLEBAR_HEIGHT) + 2 + addr_h + 2;
                int32_t col_w = 70, row_h = 50;
                bool at_root = (windows[i].explorer_path[0] == '\0' || strcmp(windows[i].explorer_path, "/") == 0);

                explorer_count = 0;
                nfs_list_path(windows[i].explorer_path, explorer_collect);
                int total_slots = (at_root ? 0 : 1) + explorer_count;
                int32_t area_x = windows[i].x + 8;
                bool hit_item = false;
                for (int slot = 0; slot < total_slots; slot++) {
                    int32_t ix, iy;
                    grid_item_rect(area_x, list_y + 6, windows[i].w - 12, col_w, row_h, slot, &ix, &iy);
                    if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 46) && !(!at_root && slot == 0)) {
                        int data_idx = at_root ? slot : slot - 1;
                        ctx_menu_open_item(mouse_x, mouse_y, i, windows[i].explorer_path,
                                            explorer_names[data_idx], explorer_is_dir[data_idx],
                                            explorer_sizes[data_idx], false);
                        hit_item = true;
                        break;
                    }
                }
                if (!hit_item) ctx_menu_open_empty(mouse_x, mouse_y, i, windows[i].explorer_path);
            }
            dirty = true;
        }

        if (!handled) {
            /* Desktop right-click: check fixed icons, then dynamic Desktop-folder icons */
            int32_t ix, iy;
            desktop_icon_rect(0, &ix, &iy);
            desktop_icon_rect(1, &ix, &iy); /* recomputed per-icon below */
            bool hit_icon = false;

            desktop_icon_rect(0, &ix, &iy);
            if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                ctx_menu_open_item(mouse_x, mouse_y, -1, "", "MY COMPUTER", true, 0, false);
                hit_icon = true;
            }
            if (!hit_icon) {
                desktop_icon_rect(1, &ix, &iy);
                if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                    ctx_menu_open_item(mouse_x, mouse_y, -1, "", "Recycle Bin", true, 0, true);
                    hit_icon = true;
                }
            }
            if (!hit_icon) {
                desktop_dyn_count = 0;
                nfs_list_path("Desktop", desktop_collect);
                for (int i = 0; i < desktop_dyn_count && !hit_icon; i++) {
                    desktop_icon_rect(DESKTOP_FIXED_COUNT + i, &ix, &iy);
                    if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                        ctx_menu_open_item(mouse_x, mouse_y, -1, "Desktop", desktop_names[i], desktop_is_dir[i], desktop_sizes[i], false);
                        hit_icon = true;
                    }
                }
            }
            if (!hit_icon) ctx_menu_open_empty(mouse_x, mouse_y, -1, "Desktop");
        }
        dirty = true;
    }

    if (click_down) {
        int32_t ty = screen_h - TASKBAR_HEIGHT;

        if (point_in_rect(mouse_x, mouse_y, 0, ty, START_BTN_WIDTH + 2, TASKBAR_HEIGHT)) {
            start_menu_open = !start_menu_open;
            if (start_menu_open) scan_software_folder();
            dirty = true;
        } else if (start_menu_open) {
            int32_t mx, my, mw, mh;
            start_menu_rect(&mx, &my, &mw, &mh);
            if (point_in_rect(mouse_x, mouse_y, mx, my, mw, mh)) {
                bool hit_discovered = false;
                for (int i = 0; i < discovered_count; i++) {
                    int32_t iy = my + i * MENU_ITEM_HEIGHT;
                    if (point_in_rect(mouse_x, mouse_y, mx, iy, mw, MENU_ITEM_HEIGHT)) {
                        wm_open_window_at(discovered_programs[i].name, COL_WINDOW_CONTENT_DEFAULT,
                                           APP_SOCKS, discovered_programs[i].path);
                        hit_discovered = true;
                        break;
                    }
                }
                for (int i = 0; i < MENU_ITEM_COUNT && !hit_discovered; i++) {
                    int32_t ix, iy, iw, ih;
                    menu_item_rect(i, &ix, &iy, &iw, &ih);
                    if (point_in_rect(mouse_x, mouse_y, ix, iy, iw, ih)) {
                        if (menu_items[i].power_action == 1) { do_shutdown(); }
                        else if (menu_items[i].power_action == 2) { do_restart(); }
                        else { wm_open_window(menu_items[i].label, COL_WINDOW_CONTENT_DEFAULT, menu_items[i].app_type); }
                        break;
                    }
                }
            }
            start_menu_open = false;
            dirty = true;
        } else {
            bool hit_taskbar_btn = false;
            for (int i = 0; i < WM_MAX_WINDOWS && !hit_taskbar_btn; i++) {
                if (!windows[i].in_use) continue;
                int32_t bx, by, bw, bh;
                taskbar_button_rect(i, &bx, &by, &bw, &bh);
                if (point_in_rect(mouse_x, mouse_y, bx, by, bw, bh)) {
                    windows[i].minimized = !windows[i].minimized;
                    if (!windows[i].minimized) z_bring_to_front(i);
                    hit_taskbar_btn = true;
                    dirty = true;
                }
            }

            if (!hit_taskbar_btn) {
                for (int zi = z_count - 1; zi >= 0; zi--) {
                    int i = z_order[zi];
                    if (!windows[i].in_use || windows[i].minimized) continue;

                    int32_t cbx, cby;
                    close_button_rect(&windows[i], &cbx, &cby);
                    if (point_in_rect(mouse_x, mouse_y, cbx, cby, BTN_SIZE, BTN_SIZE)) {
                        if (windows[i].app_type == APP_PAINT && windows[i].paint_canvas) {
                            kfree(windows[i].paint_canvas);
                            windows[i].paint_canvas = NULL;
                            if (windows[i].paint_undo_canvas) {
                                kfree(windows[i].paint_undo_canvas);
                                windows[i].paint_undo_canvas = NULL;
                            }
                        }
                        if (windows[i].app_type == APP_BROWSER && windows[i].browser_conn) {
                            tcp_close((tcp_connection_t*)windows[i].browser_conn);
                            windows[i].browser_conn = NULL;
                        }
                        windows[i].in_use = false;
                        z_remove(i);
                        dirty = true;
                        break;
                    }

                    int32_t mbx, mby;
                    minimize_button_rect(&windows[i], &mbx, &mby);
                    if (point_in_rect(mouse_x, mouse_y, mbx, mby, BTN_SIZE, BTN_SIZE)) {
                        windows[i].minimized = true;
                        dirty = true;
                        break;
                    }

                    int32_t xbx, xby;
                    maximize_button_rect(&windows[i], &xbx, &xby);
                    if (point_in_rect(mouse_x, mouse_y, xbx, xby, BTN_SIZE, BTN_SIZE)) {
                        window_t *mw = &windows[i];
                        if (mw->maximized) {
                            mw->x = mw->restore_x; mw->y = mw->restore_y;
                            mw->w = mw->restore_w; mw->h = mw->restore_h;
                            mw->maximized = false;
                        } else {
                            mw->restore_x = mw->x; mw->restore_y = mw->y;
                            mw->restore_w = mw->w; mw->restore_h = mw->h;
                            mw->x = 4; mw->y = 4;
                            mw->w = screen_w - 8;
                            mw->h = screen_h - TASKBAR_HEIGHT - 8;
                            mw->maximized = true;
                            if (mw->app_type == APP_PAINT && mw->paint_canvas) {
                                int32_t new_cw = mw->w;
                                int32_t new_ch = mw->h - TITLEBAR_HEIGHT - PAINT_TOOLBAR_H;
                                if (new_ch < 1) new_ch = 1;
                                uint32_t *nc = (uint32_t*)kmalloc((size_t)new_cw * new_ch * sizeof(uint32_t));
                                if (nc) {
                                    for (int32_t p = 0; p < new_cw * new_ch; p++) nc[p] = 0xFFFFFFFF;
                                    int32_t cw2 = (new_cw < mw->paint_canvas_w) ? new_cw : mw->paint_canvas_w;
                                    int32_t ch2 = (new_ch < mw->paint_canvas_h) ? new_ch : mw->paint_canvas_h;
                                    for (int32_t yy = 0; yy < ch2; yy++) {
                                        memcpy(nc + yy * new_cw, mw->paint_canvas + yy * mw->paint_canvas_w, (size_t)cw2 * sizeof(uint32_t));
                                    }
                                    kfree(mw->paint_canvas);
                                    mw->paint_canvas = nc;
                                    mw->paint_canvas_w = new_cw;
                                    mw->paint_canvas_h = new_ch;
                                }
                            }
                        }
                        z_bring_to_front(i);
                        dirty = true;
                        break;
                    }

                    int32_t rhx, rhy, rhw, rhh;
                    resize_handle_rect(&windows[i], &rhx, &rhy, &rhw, &rhh);
                    if (point_in_rect(mouse_x, mouse_y, rhx, rhy, rhw, rhh)) {
                        z_bring_to_front(i);
                        resizing_window = i;
                        resize_start_mouse_x = mouse_x;
                        resize_start_mouse_y = mouse_y;
                        resize_start_w = windows[i].w;
                        resize_start_h = windows[i].h;
                        dirty = true;
                        break;
                    }

                    if (point_in_rect(mouse_x, mouse_y, windows[i].x, windows[i].y, windows[i].w, TITLEBAR_HEIGHT)) {
                        z_bring_to_front(i);
                        dragging_window = i;
                        drag_offset_x = mouse_x - windows[i].x;
                        drag_offset_y = mouse_y - windows[i].y;
                        dirty = true;
                        break;
                    }

                    if (point_in_rect(mouse_x, mouse_y, windows[i].x, windows[i].y, windows[i].w, windows[i].h)) {
                        z_bring_to_front(i);
                        dirty = true;

                        int32_t local_x = mouse_x - windows[i].x;
                        int32_t local_y = mouse_y - (windows[i].y + TITLEBAR_HEIGHT);

                        if (windows[i].app_type == APP_NOTEPAD || windows[i].app_type == APP_SOCKS) {
                            if (socks_ensure_loaded(windows[i].socks_path)) {
                                socks_call_click(g_loaded_socks_header, &windows[i].socks_ctx, mouse_x, mouse_y);
                            }
                        } else if (windows[i].app_type == APP_CALCULATOR) {
                            for (int row = 0; row < CALC_GRID_ROWS; row++) {
                                for (int col = 0; col < CALC_GRID_COLS; col++) {
                                    if (calc_labels[row][col][0] == '\0') continue;
                                    int32_t bx, by;
                                    calc_button_rect(&windows[i], row, col, &bx, &by);
                                    if (point_in_rect(mouse_x, mouse_y, bx, by, CALC_BTN_W, CALC_BTN_H)) {
                                        calc_handle_button(&windows[i], calc_labels[row][col]);
                                    }
                                }
                            }
                        } else if (windows[i].app_type == APP_PAINT && windows[i].paint_canvas) {
                            window_t *pw = &windows[i];
                            bool hit_toolbar = false;

                            for (int t = 0; t < PAINT_TOOL_COUNT; t++) {
                                int32_t bx, by, bw, bh;
                                paint_tool_button_rect(pw, t, &bx, &by, &bw, &bh);
                                if (point_in_rect(mouse_x, mouse_y, bx, by, bw, bh)) {
                                    pw->paint_tool = (paint_tool_t)t;
                                    pw->paint_text_active = false;
                                    hit_toolbar = true;
                                }
                            }
                            for (int p = 0; p < PAINT_PALETTE_COUNT; p++) {
                                int32_t bx, by, bw, bh;
                                paint_swatch_rect(pw, p, &bx, &by, &bw, &bh);
                                if (point_in_rect(mouse_x, mouse_y, bx, by, bw, bh)) {
                                    pw->paint_color = PAINT_PALETTE[p];
                                    hit_toolbar = true;
                                }
                            }

                            int32_t ubx, uby, ubw, ubh;
                            paint_undo_button_rect(pw, &ubx, &uby, &ubw, &ubh);
                            if (point_in_rect(mouse_x, mouse_y, ubx, uby, ubw, ubh)) {
                                if (pw->paint_undo_canvas && pw->paint_undo_w == pw->paint_canvas_w && pw->paint_undo_h == pw->paint_canvas_h) {
                                    uint32_t *tmp = pw->paint_canvas;
                                    pw->paint_canvas = pw->paint_undo_canvas;
                                    pw->paint_undo_canvas = tmp;
                                }
                                hit_toolbar = true;
                            }

                            int32_t sbx, sby, sbw, sbh;
                            paint_save_button_rect(pw, &sbx, &sby, &sbw, &sbh);
                            if (point_in_rect(mouse_x, mouse_y, sbx, sby, sbw, sbh)) {
                                if (pw->paint_path[0]) {
                                    static uint8_t bmp_save_buf[NFS_MAX_FILE_SIZE];
                                    int32_t sz = bmp_encode(pw->paint_canvas, pw->paint_canvas_w, pw->paint_canvas_h, bmp_save_buf, sizeof(bmp_save_buf));
                                    if (sz > 0) { nfs_write_path(pw->paint_path, bmp_save_buf, (uint32_t)sz); invalidate_all_explorer_caches(); }
                                } else {
                                    pw->paint_saving_as = true;
                                    pw->paint_saveas_buf[0] = '\0';
                                    pw->paint_saveas_len = 0;
                                }
                                hit_toolbar = true;
                            }
                            int32_t abx2, aby2, abw2, abh2;
                            paint_saveas_button_rect(pw, &abx2, &aby2, &abw2, &abh2);
                            if (point_in_rect(mouse_x, mouse_y, abx2, aby2, abw2, abh2)) {
                                pw->paint_saving_as = true;
                                strncpy(pw->paint_saveas_buf, pw->paint_path, sizeof(pw->paint_saveas_buf) - 1);
                                pw->paint_saveas_len = (int32_t)strlen(pw->paint_saveas_buf);
                                hit_toolbar = true;
                            }

                            if (!hit_toolbar) {
                                int32_t canvas_local_y = local_y - PAINT_TOOLBAR_H;
                                if (local_x >= 0 && local_x < pw->paint_canvas_w &&
                                    canvas_local_y >= 0 && canvas_local_y < pw->paint_canvas_h) {

                                    /* snapshot for undo before any canvas-modifying action */
                                    if (!pw->paint_undo_canvas || pw->paint_undo_w != pw->paint_canvas_w || pw->paint_undo_h != pw->paint_canvas_h) {
                                        if (pw->paint_undo_canvas) kfree(pw->paint_undo_canvas);
                                        pw->paint_undo_canvas = (uint32_t*)kmalloc((size_t)pw->paint_canvas_w * pw->paint_canvas_h * sizeof(uint32_t));
                                        pw->paint_undo_w = pw->paint_canvas_w;
                                        pw->paint_undo_h = pw->paint_canvas_h;
                                    }
                                    if (pw->paint_undo_canvas) {
                                        memcpy(pw->paint_undo_canvas, pw->paint_canvas,
                                               (size_t)pw->paint_canvas_w * pw->paint_canvas_h * sizeof(uint32_t));
                                    }

                                    if (pw->paint_tool == PAINT_TOOL_BUCKET) {
                                        paint_flood_fill(pw, local_x, canvas_local_y, pw->paint_color);
                                    } else if (pw->paint_tool == PAINT_TOOL_LINE || pw->paint_tool == PAINT_TOOL_RECT ||
                                               pw->paint_tool == PAINT_TOOL_ELLIPSE) {
                                        pw->paint_shape_start_x = local_x;
                                        pw->paint_shape_start_y = canvas_local_y;
                                        pw->paint_shape_dragging = true;
                                        painting_window = i;
                                    } else if (pw->paint_tool == PAINT_TOOL_TEXT) {
                                        pw->paint_text_active = true;
                                        pw->paint_text_x = local_x;
                                        pw->paint_text_y = canvas_local_y;
                                    } else {
                                        uint32_t draw_color = (pw->paint_tool == PAINT_TOOL_ERASER)
                                                                ? fb_pack_color(255, 255, 255) : pw->paint_color;
                                        paint_draw_line(pw, local_x, canvas_local_y, local_x, canvas_local_y, draw_color);
                                        painting_window = i;
                                        pw->paint_last_x = local_x;
                                        pw->paint_last_y = canvas_local_y;
                                    }
                                }
                            }
                        } else if (windows[i].app_type == APP_BROWSER) {
                            int32_t abx, aby, abw, abh;
                            browser_addr_bar_rect(&windows[i], &abx, &aby, &abw, &abh);
                            int32_t gbx, gby, gbw, gbh;
                            browser_go_button_rect(&windows[i], &gbx, &gby, &gbw, &gbh);
                            int32_t bbx, bby, bbw, bbh;
                            browser_back_button_rect(&windows[i], &bbx, &bby, &bbw, &bbh);
                            int32_t fbx, fby, fbw, fbh;
                            browser_fwd_button_rect(&windows[i], &fbx, &fby, &fbw, &fbh);

                            if (point_in_rect(mouse_x, mouse_y, abx, aby, abw, abh)) {
                                windows[i].browser_editing_url = true;
                            } else if (point_in_rect(mouse_x, mouse_y, gbx, gby, gbw, gbh)) {
                                windows[i].browser_editing_url = false;
                                browser_navigate(&windows[i]);
                            } else if (point_in_rect(mouse_x, mouse_y, bbx, bby, bbw, bbh)) {
                                windows[i].browser_editing_url = false;
                                browser_go_back(&windows[i]);
                            } else if (point_in_rect(mouse_x, mouse_y, fbx, fby, fbw, fbh)) {
                                windows[i].browser_editing_url = false;
                                browser_go_forward(&windows[i]);
                            } else {
                                windows[i].browser_editing_url = false;
                                /* Check for a click on a rendered link line */
                                int32_t content_y2 = windows[i].y + TITLEBAR_HEIGHT + BROWSER_BAR_H + 4;
                                int32_t status_offset = (windows[i].browser_state == 1 || windows[i].browser_state == 2 ||
                                                          windows[i].browser_state == 4) ? font_char_height(FONT_SCALE) + 6 : 0;
                                int32_t text_y2 = content_y2 + 4 + status_offset;
                                for (int32_t li = 0; li < windows[i].browser_line_count; li++) {
                                    if (windows[i].browser_line_link[li][0] == '\0') continue;
                                    int32_t ly, lh;
                                    browser_line_rect(windows[i].x + 8, text_y2, li, &ly, &lh);
                                    if (point_in_rect(mouse_x, mouse_y, windows[i].x + 8, ly, windows[i].w - 16, lh)) {
                                        const char *link = windows[i].browser_line_link[li];
                                        char target[128];
                                        if (link[0] == '/') {
                                            /* relative to the current host */
                                            char host_only[64];
                                            strncpy(host_only, windows[i].browser_url, sizeof(host_only) - 1);
                                            host_only[sizeof(host_only) - 1] = '\0';
                                            for (char *p = host_only; *p; p++) { if (*p == '/') { *p = '\0'; break; } }
                                            strncpy(target, host_only, sizeof(target) - 1);
                                            target[sizeof(target) - 1] = '\0';
                                            strncat(target, link, sizeof(target) - strlen(target) - 1);
                                        } else {
                                            const char *rest = link;
                                            if (strncmp(rest, "http://", 7) == 0) rest += 7;
                                            strncpy(target, rest, sizeof(target) - 1);
                                            target[sizeof(target) - 1] = '\0';
                                        }
                                        strncpy(windows[i].browser_url, target, sizeof(windows[i].browser_url) - 1);
                                        windows[i].browser_url[sizeof(windows[i].browser_url) - 1] = '\0';
                                        windows[i].browser_url_len = (int32_t)strlen(windows[i].browser_url);
                                        browser_navigate(&windows[i]);
                                        break;
                                    }
                                }
                            }
                        } else if (windows[i].app_type == APP_EXPLORER) {
                            int32_t addr_h = 22;
                            int32_t list_y = (windows[i].y + TITLEBAR_HEIGHT) + 2 + addr_h + 2;
                            int32_t col_w = 70, row_h = 50;
                            bool at_root = (windows[i].explorer_path[0] == '\0' || strcmp(windows[i].explorer_path, "/") == 0);

                            explorer_count = 0;
                            nfs_list_path(windows[i].explorer_path, explorer_collect);

                            int total_slots = (at_root ? 0 : 1) + explorer_count;
                            int32_t area_x = windows[i].x + 8;
                            for (int slot = 0; slot < total_slots; slot++) {
                                int32_t ix, iy;
                                grid_item_rect(area_x, list_y + 6, windows[i].w - 12, col_w, row_h, slot, &ix, &iy);
                                if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 46)) {
                                    if (!at_root && slot == 0) {
                                        char parent[NFS_MAX_PATH];
                                        path_parent(windows[i].explorer_path, parent, sizeof(parent));
                                        strncpy(windows[i].explorer_path, parent, sizeof(windows[i].explorer_path) - 1);
                                        windows[i].explorer_path[sizeof(windows[i].explorer_path) - 1] = '\0';
                                    } else {
                                        int data_idx = at_root ? slot : slot - 1;
                                        dnd_candidate = true;
                                        dnd_active = false;
                                        strncpy(dnd_name, explorer_names[data_idx], sizeof(dnd_name) - 1);
                                        dnd_name[sizeof(dnd_name) - 1] = '\0';
                                        dnd_is_dir = explorer_is_dir[data_idx];
                                        dnd_src_window = i;
                                        strncpy(dnd_src_dir, windows[i].explorer_path, sizeof(dnd_src_dir) - 1);
                                        dnd_src_dir[sizeof(dnd_src_dir) - 1] = '\0';
                                        dnd_start_x = mouse_x;
                                        dnd_start_y = mouse_y;
                                    }
                                    break;
                                }
                            }
                        }
                        break;
                    }
                }

                bool click_in_any_window = false;
                for (int wi = 0; wi < WM_MAX_WINDOWS; wi++) {
                    if (windows[wi].in_use && !windows[wi].minimized &&
                        point_in_rect(mouse_x, mouse_y, windows[wi].x - 2, windows[wi].y - 2,
                                      windows[wi].w + 4, windows[wi].h + 4)) {
                        click_in_any_window = true;
                        break;
                    }
                }

                if (!click_in_any_window) {
                    int32_t ix, iy;
                    desktop_icon_rect(0, &ix, &iy);
                    if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                        wm_open_window_at("MY COMPUTER", COL_WINDOW_CONTENT_DEFAULT, APP_EXPLORER, "/");
                        dirty = true;
                    } else {
                        desktop_icon_rect(1, &ix, &iy);
                        if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                            wm_open_window_at("RECYCLE BIN", COL_WINDOW_CONTENT_DEFAULT, APP_EXPLORER, "Recycle Bin");
                            dirty = true;
                        } else {
                            desktop_dyn_count = 0;
                            nfs_list_path("Desktop", desktop_collect);
                            for (int di = 0; di < desktop_dyn_count; di++) {
                                desktop_icon_rect(DESKTOP_FIXED_COUNT + di, &ix, &iy);
                                if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                                    dnd_candidate = true;
                                    dnd_active = false;
                                    strncpy(dnd_name, desktop_names[di], sizeof(dnd_name) - 1);
                                    dnd_name[sizeof(dnd_name) - 1] = '\0';
                                    dnd_is_dir = desktop_is_dir[di];
                                    dnd_src_window = -1;
                                    strncpy(dnd_src_dir, "Desktop", sizeof(dnd_src_dir) - 1);
                                    dnd_src_dir[sizeof(dnd_src_dir) - 1] = '\0';
                                    dnd_start_x = mouse_x;
                                    dnd_start_y = mouse_y;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (mouse_left_down && dragging_window >= 0) {
        int32_t old_y = windows[dragging_window].y;
        int32_t h = windows[dragging_window].h;
        windows[dragging_window].x = mouse_x - drag_offset_x;
        windows[dragging_window].y = mouse_y - drag_offset_y;
        if (windows[dragging_window].y < 0) windows[dragging_window].y = 0;
        if (windows[dragging_window].y > screen_h - TASKBAR_HEIGHT - TITLEBAR_HEIGHT)
            windows[dragging_window].y = screen_h - TASKBAR_HEIGHT - TITLEBAR_HEIGHT;
        int32_t new_y = windows[dragging_window].y;
        /* +6/-6 margin for the drop-shadow/bevel draw_window paints just outside the window rect */
        expand_dirty_rows(old_y - 6, old_y + h + 6);
        expand_dirty_rows(new_y - 6, new_y + h + 6);
        expand_dirty_rows(mouse_y - 4, mouse_y + 20); /* rough cursor bounds, so it isn't left stale */
        dirty = true;
    }

    if (mouse_left_down && dnd_candidate && !dnd_active) {
        int32_t ddx = mouse_x - dnd_start_x, ddy = mouse_y - dnd_start_y;
        if (ddx * ddx + ddy * ddy > 144) { /* moved more than ~12px: this is a drag, not a click */
            dnd_active = true;
        }
    }

    if (mouse_left_down && resizing_window >= 0) {
        window_t *rw = &windows[resizing_window];
        int32_t new_w = resize_start_w + (mouse_x - resize_start_mouse_x);
        int32_t new_h = resize_start_h + (mouse_y - resize_start_mouse_y);
        if (new_w < 180) new_w = 180;
        if (new_h < 120) new_h = 120;
        if (rw->x + new_w > screen_w) new_w = screen_w - rw->x;
        if (rw->y + new_h > screen_h - TASKBAR_HEIGHT) new_h = screen_h - TASKBAR_HEIGHT - rw->y;
        rw->w = new_w;
        rw->h = new_h;
        dirty = true;
    }

    if (mouse_left_down && painting_window >= 0) {
        window_t *pw = &windows[painting_window];
        if (pw->in_use && pw->paint_canvas) {
            int32_t local_x = mouse_x - pw->x;
            int32_t local_y = mouse_y - (pw->y + TITLEBAR_HEIGHT) - PAINT_TOOLBAR_H;
            if (local_x >= 0 && local_x < pw->paint_canvas_w && local_y >= 0 && local_y < pw->paint_canvas_h) {
                if (pw->paint_shape_dragging && pw->paint_undo_canvas &&
                    pw->paint_undo_w == pw->paint_canvas_w && pw->paint_undo_h == pw->paint_canvas_h) {
                    /* live preview: start from the pre-drag snapshot each frame, then draw the
                       shape from its start point to wherever the mouse is right now */
                    memcpy(pw->paint_canvas, pw->paint_undo_canvas,
                           (size_t)pw->paint_canvas_w * pw->paint_canvas_h * sizeof(uint32_t));
                    if (pw->paint_tool == PAINT_TOOL_LINE) {
                        paint_draw_line(pw, pw->paint_shape_start_x, pw->paint_shape_start_y, local_x, local_y, pw->paint_color);
                    } else if (pw->paint_tool == PAINT_TOOL_RECT) {
                        paint_draw_rect(pw, pw->paint_shape_start_x, pw->paint_shape_start_y, local_x, local_y, pw->paint_color);
                    } else if (pw->paint_tool == PAINT_TOOL_ELLIPSE) {
                        paint_draw_ellipse(pw, pw->paint_shape_start_x, pw->paint_shape_start_y, local_x, local_y, pw->paint_color);
                    }
                } else if (!pw->paint_shape_dragging) {
                    uint32_t draw_color = (pw->paint_tool == PAINT_TOOL_ERASER)
                                            ? fb_pack_color(255, 255, 255) : pw->paint_color;
                    int32_t from_x = (pw->paint_last_x >= 0) ? pw->paint_last_x : local_x;
                    int32_t from_y = (pw->paint_last_y >= 0) ? pw->paint_last_y : local_y;
                    paint_draw_line(pw, from_x, from_y, local_x, local_y, draw_color);
                    pw->paint_last_x = local_x;
                    pw->paint_last_y = local_y;
                }
            }
            expand_dirty_rows(pw->y - 8, pw->y + pw->h + 8);
            dirty = true;
        }
    }

    for (int i = 0; i < WM_MAX_WINDOWS; i++) {
        if (windows[i].in_use && windows[i].app_type == APP_BROWSER) {
            browser_tick(&windows[i], &dirty);
        }
    }

    if (click_up) {
        if (dnd_candidate) {
            if (dnd_active) {
                int32_t ix, iy;
                bool dropped = false;

                desktop_icon_rect(1, &ix, &iy);
                if (!dropped && point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                    nfs_move_path(dnd_src_dir, dnd_name, "Recycle Bin");
                    dropped = true;
                }

                if (!dropped) {
                    desktop_dyn_count = 0;
                    nfs_list_path("Desktop", desktop_collect);
                    for (int di = 0; di < desktop_dyn_count; di++) {
                        if (!desktop_is_dir[di]) continue;
                        if (dnd_src_window == -1 && strcmp(desktop_names[di], dnd_name) == 0) continue;
                        desktop_icon_rect(DESKTOP_FIXED_COUNT + di, &ix, &iy);
                        if (point_in_rect(mouse_x, mouse_y, ix, iy, 32, 60)) {
                            char dst_dir[NFS_MAX_PATH];
                            path_join("Desktop", desktop_names[di], dst_dir, sizeof(dst_dir));
                            nfs_move_path(dnd_src_dir, dnd_name, dst_dir);
                            dropped = true;
                            break;
                        }
                    }
                }

                /* Dropping onto an open Explorer window - either onto a folder
                   item inside it (moves in there), or onto its general content
                   area (moves into whatever folder that window is showing). */
                if (!dropped) {
                    for (int wi = 0; wi < WM_MAX_WINDOWS && !dropped; wi++) {
                        window_t *ew = &windows[wi];
                        if (!ew->in_use || ew->app_type != APP_EXPLORER || ew->minimized) continue;
                        int32_t cx = ew->x, cy = ew->y + TITLEBAR_HEIGHT;
                        int32_t cw = ew->w, ch = ew->h - TITLEBAR_HEIGHT;
                        if (!point_in_rect(mouse_x, mouse_y, cx, cy, cw, ch)) continue;

                        bool at_root = (ew->explorer_path[0] == '\0' || strcmp(ew->explorer_path, "/") == 0);
                        int32_t addr_h = 22;
                        int32_t list_y = cy + 2 + addr_h + 2;
                        int32_t col_w = 70, row_h = 50;
                        int32_t area_x = cx + 8;
                        int total_slots = (at_root ? 0 : 1) + ew->explorer_cache_count;
                        bool on_item = false;

                        for (int slot = 0; slot < total_slots; slot++) {
                            int32_t ix2, iy2;
                            grid_item_rect(area_x, list_y + 6, cw - 12, col_w, row_h, slot, &ix2, &iy2);
                            if (point_in_rect(mouse_x, mouse_y, ix2, iy2, 32, 46)) {
                                on_item = true;
                                if (!at_root && slot == 0) break; /* ".." isn't a valid drop target */
                                int di = at_root ? slot : slot - 1;
                                if (di < ew->explorer_cache_count && ew->explorer_cache_is_dir[di] &&
                                    !(dnd_src_window == wi && strcmp(ew->explorer_cache_names[di], dnd_name) == 0)) {
                                    char dst_dir[NFS_MAX_PATH];
                                    path_join(ew->explorer_path, ew->explorer_cache_names[di], dst_dir, sizeof(dst_dir));
                                    nfs_move_path(dnd_src_dir, dnd_name, dst_dir);
                                    dropped = true;
                                }
                                break;
                            }
                        }
                        if (!dropped && !on_item && strcmp(ew->explorer_path, dnd_src_dir) != 0) {
                            nfs_move_path(dnd_src_dir, dnd_name, ew->explorer_path);
                            dropped = true;
                        }
                    }
                }
            } else {
                open_item(dnd_src_dir, dnd_name, dnd_is_dir, dnd_src_window);
            }
            invalidate_all_explorer_caches();
            dnd_candidate = false;
            dnd_active = false;
            dnd_src_window = -1;
            force_redraw = true;
        }
        if (painting_window >= 0 && windows[painting_window].in_use) {
            windows[painting_window].paint_last_x = -1;
            windows[painting_window].paint_last_y = -1;
            windows[painting_window].paint_shape_dragging = false;
        }
        if (resizing_window >= 0 && windows[resizing_window].in_use) {
            window_t *rw = &windows[resizing_window];
            if (rw->app_type == APP_PAINT) {
                int32_t new_cw = rw->w;
                int32_t new_ch = rw->h - TITLEBAR_HEIGHT - PAINT_TOOLBAR_H;
                if (new_ch < 1) new_ch = 1;
                uint32_t *new_canvas = (uint32_t*)kmalloc((size_t)new_cw * new_ch * sizeof(uint32_t));
                if (new_canvas) {
                    for (int32_t p = 0; p < new_cw * new_ch; p++) new_canvas[p] = 0xFFFFFFFF;
                    if (rw->paint_canvas) {
                        int32_t copy_w = (new_cw < rw->paint_canvas_w) ? new_cw : rw->paint_canvas_w;
                        int32_t copy_h = (new_ch < rw->paint_canvas_h) ? new_ch : rw->paint_canvas_h;
                        for (int32_t yy = 0; yy < copy_h; yy++) {
                            memcpy(new_canvas + yy * new_cw,
                                   rw->paint_canvas + yy * rw->paint_canvas_w,
                                   (size_t)copy_w * sizeof(uint32_t));
                        }
                        kfree(rw->paint_canvas);
                    }
                    rw->paint_canvas = new_canvas;
                    rw->paint_canvas_w = new_cw;
                    rw->paint_canvas_h = new_ch;
                }
            }
        }
        dragging_window = -1;
        resizing_window = -1;
        painting_window = -1;
    }

    prev_mouse_left = mouse_left_down;
    prev_mouse_right = mouse_right_down;

    bool in_continuous_op = mouse_left_down &&
                             (dragging_window >= 0 || resizing_window >= 0 || painting_window >= 0);
    bool force_immediate_redraw = !in_continuous_op;

    if (dirty) {
        uint64_t now = timer_ticks();
        if (now - last_redraw_tick >= REDRAW_MIN_INTERVAL_TICKS || force_immediate_redraw) {
            bool partial = ((dragging_window >= 0 || painting_window >= 0) && dirty_rect_y0 >= 0);
            if (partial) {
                redraw_partial(dirty_rect_y0, dirty_rect_y1);
            } else {
                redraw_all();
            }
            cursor_reset(mouse_x, mouse_y);
            uint64_t tp0 = timer_ticks();
            if (partial) {
                fb_present_rows(dirty_rect_y0, dirty_rect_y1);
            } else {
                fb_present();
            }
            uint64_t tp1 = timer_ticks();
            static int present_dbg_counter = 0;
            if (++present_dbg_counter % 30 == 0) log_ticks("[perf] present", tp1 - tp0);
            last_redraw_tick = now;
            dirty_rect_y0 = -1;
            dirty_rect_y1 = -1;

            redraws_this_window++;
            if (now - rps_window_start >= 100) { /* ~1 real second */
                last_rps = redraws_this_window;
                redraws_this_window = 0;
                rps_window_start = now;
                char b[32]; int bi=0; const char*p="[wm] redraws/sec="; while(*p)b[bi++]=*p++;
                int n=last_rps; char d[6]; int di=0;
                if (n==0) d[di++]='0';
                while (n>0) { d[di++]=(char)('0'+n%10); n/=10; }
                while (di>0) b[bi++]=d[--di];
                b[bi++]='\n'; b[bi]=0; serial_write(b);
            }
        }
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
    } else if (mouse_x != last_mouse_x || mouse_y != last_mouse_y) {
        cursor_move_to(mouse_x, mouse_y);
        last_mouse_x = mouse_x;
        last_mouse_y = mouse_y;
        fb_present();
    }
}
