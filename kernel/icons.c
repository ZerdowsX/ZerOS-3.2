#include "icons.h"
#include "fb.h"

/* Real (PNG-sourced) icon set, embedded as raw RGBA at two sizes - 32x32
   (used at scale=2, our common desktop/Explorer icon size) and 16x16
   (used at scale=1, for smaller UI spots like the address bar). Alpha
   blending (fb_blit_rgba) gives clean anti-aliased edges instead of the
   old flat-color, hard-edged pixel art. */
extern const uint8_t icon_folder_32[];
extern const uint8_t icon_folder_16[];
extern const uint8_t icon_disk_32[];
extern const uint8_t icon_disk_16[];
extern const uint8_t icon_notepad_32[];
extern const uint8_t icon_notepad_16[];
extern const uint8_t icon_file_32[];
extern const uint8_t icon_file_16[];
extern const uint8_t icon_paint_32[];
extern const uint8_t icon_paint_16[];
extern const uint8_t icon_terminal_32[];
extern const uint8_t icon_terminal_16[];
extern const uint8_t icon_browser_32[];
extern const uint8_t icon_browser_16[];
extern const uint8_t icon_calculator_32[];
extern const uint8_t icon_calculator_16[];
extern const uint8_t icon_trash_full_32[];
extern const uint8_t icon_trash_full_16[];
extern const uint8_t icon_trash_empty_32[];
extern const uint8_t icon_trash_empty_16[];

static void icon_lookup(icon_id_t icon, const uint8_t **data32, const uint8_t **data16) {
    switch (icon) {
        case ICON_FOLDER:      *data32 = icon_folder_32;      *data16 = icon_folder_16;      break;
        case ICON_DISK:        *data32 = icon_disk_32;        *data16 = icon_disk_16;        break;
        case ICON_NOTEPAD:     *data32 = icon_notepad_32;     *data16 = icon_notepad_16;     break;
        case ICON_CALCULATOR:  *data32 = icon_calculator_32;  *data16 = icon_calculator_16;  break;
        case ICON_PAINT:       *data32 = icon_paint_32;       *data16 = icon_paint_16;       break;
        case ICON_FILE:        *data32 = icon_file_32;        *data16 = icon_file_16;        break;
        case ICON_BROWSER:     *data32 = icon_browser_32;     *data16 = icon_browser_16;     break;
        case ICON_TRASH:       *data32 = icon_trash_empty_32; *data16 = icon_trash_empty_16; break;
        case ICON_TRASH_FULL:  *data32 = icon_trash_full_32;  *data16 = icon_trash_full_16;  break;
        case ICON_TERMINAL:    *data32 = icon_terminal_32;    *data16 = icon_terminal_16;    break;
        default:               *data32 = icon_file_32;        *data16 = icon_file_16;        break;
    }
}

/* icon_draw's contract is "16x16 logical icon, each icon-pixel drawn as
   scale x scale" - our real icons are natively 32x32 or 16x16, so scale=2
   maps directly to the 32x32 source (blitted 1:1, no resampling needed)
   and scale=1 maps directly to the 16x16 source. Any other scale falls
   back to the 32x32 source stretched via nearest-neighbor. */
void icon_draw(int32_t x, int32_t y, icon_id_t icon, int32_t scale) {
    const uint8_t *data32, *data16;
    icon_lookup(icon, &data32, &data16);

    if (scale == 2) {
        fb_blit_rgba(x, y, 32, 32, data32, 32);
    } else if (scale == 1) {
        fb_blit_rgba(x, y, 16, 16, data16, 16);
    } else {
        /* Uncommon path: nearest-neighbor sample the 32x32 source to whatever size is asked for. */
        int32_t size = 16 * scale;
        for (int32_t ry = 0; ry < size; ry++) {
            for (int32_t rx = 0; rx < size; rx++) {
                int32_t sx = rx * 32 / size, sy = ry * 32 / size;
                const uint8_t *px = data32 + ((size_t)sy * 32 + sx) * 4;
                uint8_t a = px[3];
                if (a == 0) continue;
                fb_put_pixel(x + rx, y + ry, fb_pack_color(px[0], px[1], px[2]));
            }
        }
    }
}
