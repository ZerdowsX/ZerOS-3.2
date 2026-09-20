#ifndef NUGGET_CURSOR_H
#define NUGGET_CURSOR_H
#include "types.h"

void cursor_init(void);
/* Erases the cursor at its last drawn position, then draws it at (x, y). */
void cursor_move_to(int32_t x, int32_t y);
/* Call after a full-screen redraw (e.g. window moved/opened) so the cursor's
   background snapshot doesn't restore stale pixels next time it moves. */
void cursor_reset(int32_t x, int32_t y);

#endif
