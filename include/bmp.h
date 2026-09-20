#ifndef NUGGET_BMP_H
#define NUGGET_BMP_H
#include "types.h"

/* Encodes a canvas of device-native packed pixels (as produced by
   fb_pack_color) into a standard uncompressed 24-bit BMP file in `out`.
   Returns the number of bytes written, or -1 if it doesn't fit in out_cap. */
int32_t bmp_encode(const uint32_t *canvas, int32_t cw, int32_t ch, uint8_t *out, int32_t out_cap);

/* Decodes an uncompressed 24-bit BMP from `data` into `out_canvas` (device-
   native packed pixels, via fb_pack_color), clipping to max_w/max_h if the
   BMP is larger. Returns true on success. */
bool bmp_decode(const uint8_t *data, int32_t data_len, uint32_t *out_canvas,
                 int32_t max_w, int32_t max_h, int32_t *out_w, int32_t *out_h);

#endif
