#ifndef NUGGET_WM_H
#define NUGGET_WM_H
#include "types.h"

#define WM_MAX_WINDOWS 8
#define WM_TITLE_MAX 24

typedef enum {
    APP_EMPTY,
    APP_NOTEPAD,
    APP_CALCULATOR,
    APP_PAINT,
    APP_EXPLORER,
    APP_BROWSER,
    APP_TERMINAL,
    APP_SOCKS, /* generic .socks program, discovered from software/ - see window_t.socks_path */
} app_type_t;

void wm_init(void);

/* Call once per main-loop iteration with the latest mouse state and whether
   the left button is currently held down. Handles clicks, dragging, the
   Start menu, and redraws whatever changed. */
void wm_update(int32_t mouse_x, int32_t mouse_y, bool mouse_left_down, bool mouse_right_down);

/* Forwards a typed character to the focused window if it accepts keyboard
   input (currently just Notepad). Call from the main loop's keyboard drain. */
void wm_handle_key(char c);

/* Opens a new window of the given app type. */
int wm_open_window(const char *title, uint32_t content_color, app_type_t app_type);
/* Same, but also gives Explorer a starting folder or Notepad a file to load. */
int wm_open_window_at(const char *title, uint32_t content_color, app_type_t app_type, const char *path);

#endif
