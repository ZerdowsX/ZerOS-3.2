#ifndef NUGGET_MOUSE_H
#define NUGGET_MOUSE_H
#include "types.h"

typedef struct {
    int32_t x, y;           /* absolute position, clamped to screen bounds */
    int8_t  dx, dy;          /* last relative movement */
    bool    left, right, middle;
} mouse_state_t;

void mouse_init(void);
mouse_state_t mouse_get_state(void);
void mouse_set_bounds(int32_t width, int32_t height);

#endif
