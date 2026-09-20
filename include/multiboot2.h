#ifndef NUGGET_MULTIBOOT2_H
#define NUGGET_MULTIBOOT2_H
#include "types.h"

typedef struct PACKED {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_header_t;

typedef struct PACKED {
    uint32_t type;
    uint32_t size;
} mb2_tag_t;

#define MB2_TAG_MEMORY_MAP 6
#define MB2_TAG_END        0

typedef struct PACKED {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* followed by entries */
} mb2_tag_mmap_t;

typedef struct PACKED {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;   /* 1 = available RAM */
    uint32_t reserved;
} mb2_mmap_entry_t;

#define MB2_MEMORY_AVAILABLE 1

#define MB2_TAG_FRAMEBUFFER 8

typedef struct PACKED {
    uint32_t type;      /* = 8 */
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;   /* 0 = indexed, 1 = RGB direct, 2 = EGA text */
    uint16_t reserved;
    /* if fb_type == 1 (RGB), followed by: */
    uint8_t  red_field_position;
    uint8_t  red_mask_size;
    uint8_t  green_field_position;
    uint8_t  green_mask_size;
    uint8_t  blue_field_position;
    uint8_t  blue_mask_size;
} mb2_tag_framebuffer_t;

#endif
