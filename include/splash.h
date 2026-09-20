#ifndef NUGGET_SPLASH_H
#define NUGGET_SPLASH_H

#include "types.h"

/* Draws the boot logo centered on a black background. Call once fb_init()
   has succeeded; call splash_clear() right before switching to the desktop. */
void splash_show(void);
/* Updates the progress bar under the logo (0-100). Safe to call repeatedly
   as boot advances through its stages. */
void splash_set_progress(int32_t percent);
void splash_clear(void);

#endif
