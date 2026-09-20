#ifndef NUGGET_WALLPAPER_H
#define NUGGET_WALLPAPER_H
#include "types.h"

/* Blits the embedded desktop wallpaper (1024x768 RGB888) to the framebuffer. */
void wallpaper_draw(void);

/* Same, but only for rows [y0,y1) - used for partial redraws (e.g. while
   dragging a window) where the rest of the wallpaper hasn't changed. */
void wallpaper_draw_rows(int32_t y0, int32_t y1);

/* Loads a .bmp file from NuggetFS and uses it as the wallpaper from now on.
   Also remembers the choice (in wallpaper.cfg) so it's restored automatically
   on the next boot via wallpaper_restore_saved_choice(). */
bool wallpaper_set_from_file(const char *path);

/* Called once at boot (after NuggetFS is mounted) to reapply whatever
   wallpaper was last chosen via "Set as Wallpaper" - does nothing if
   none was ever set. */
void wallpaper_restore_saved_choice(void);

#endif
