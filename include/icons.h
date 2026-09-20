#ifndef NUGGET_ICONS_H
#define NUGGET_ICONS_H
#include "types.h"

typedef enum {
    ICON_FOLDER,
    ICON_DISK,
    ICON_NOTEPAD,
    ICON_CALCULATOR,
    ICON_PAINT,
    ICON_FILE,
    ICON_BROWSER,
    ICON_TRASH,       /* empty */
    ICON_TRASH_FULL,
    ICON_TERMINAL,
} icon_id_t;

/* Draws a 16x16 icon at (x, y), each icon-pixel rendered as (scale x scale). */
void icon_draw(int32_t x, int32_t y, icon_id_t icon, int32_t scale);

#endif
