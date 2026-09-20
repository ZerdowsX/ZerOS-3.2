#include "pmm.h"
#include "multiboot2.h"
#include "string.h"
#include "serial.h"
#include "vga.h"

/* Bitmap covering up to 4GiB of physical memory (1 bit per 4KiB frame). */
#define MAX_FRAMES (4ULL * 1024 * 1024 * 1024 / PAGE_SIZE)
static uint8_t bitmap[MAX_FRAMES / 8];

static uint64_t total_frames = 0;
static uint64_t used_frames  = 0;
static uint64_t highest_frame = 0;

static inline void bitmap_set(uint64_t bit)   { bitmap[bit / 8] |= (1 << (bit % 8)); }
static inline void bitmap_clear(uint64_t bit) { bitmap[bit / 8] &= ~(1 << (bit % 8)); }
static inline int  bitmap_test(uint64_t bit)  { return bitmap[bit / 8] & (1 << (bit % 8)); }

void pmm_init(uint32_t mb2_info_addr, uint64_t kernel_end) {
    memset(bitmap, 0xFF, sizeof(bitmap)); /* mark everything used by default */

    uint8_t *ptr = (uint8_t*)(uint64_t)mb2_info_addr;
    mb2_info_header_t *hdr = (mb2_info_header_t*)ptr;
    uint8_t *tag_ptr = ptr + sizeof(mb2_info_header_t);
    uint8_t *end = ptr + hdr->total_size;

    while (tag_ptr < end) {
        mb2_tag_t *tag = (mb2_tag_t*)tag_ptr;
        if (tag->type == MB2_TAG_END) break;

        if (tag->type == MB2_TAG_MEMORY_MAP) {
            mb2_tag_mmap_t *mmap = (mb2_tag_mmap_t*)tag;
            uint8_t *entry_ptr = tag_ptr + sizeof(mb2_tag_mmap_t);
            uint8_t *mmap_end = tag_ptr + tag->size;

            while (entry_ptr < mmap_end) {
                mb2_mmap_entry_t *entry = (mb2_mmap_entry_t*)entry_ptr;
                if (entry->type == MB2_MEMORY_AVAILABLE) {
                    uint64_t start_frame = entry->base_addr / PAGE_SIZE;
                    uint64_t frame_count = entry->length / PAGE_SIZE;
                    for (uint64_t i = 0; i < frame_count; i++) {
                        uint64_t frame = start_frame + i;
                        if (frame < MAX_FRAMES) {
                            bitmap_clear(frame);
                            total_frames++;
                            if (frame > highest_frame) highest_frame = frame;
                        }
                    }
                }
                entry_ptr += mmap->entry_size;
            }
        }
        tag_ptr += (tag->size + 7) & ~7; /* tags are 8-byte aligned */
    }

    /* Reserve the first 1MiB (BIOS/boot structures) and the kernel image itself */
    uint64_t reserved_frames = (kernel_end / PAGE_SIZE) + 1;
    for (uint64_t i = 0; i < reserved_frames; i++) {
        if (!bitmap_test(i)) { bitmap_set(i); used_frames++; }
    }

    serial_write("[pmm] initialized\n");
    kprintf("[pmm] %u frames available (%u MB)\n",
            (unsigned)(total_frames - used_frames),
            (unsigned)((total_frames - used_frames) * PAGE_SIZE / (1024*1024)));
}

void *pmm_alloc_frame(void) {
    for (uint64_t i = 0; i <= highest_frame; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
            return (void*)(i * PAGE_SIZE);
        }
    }
    return NULL; /* out of memory */
}

void pmm_free_frame(void *frame) {
    uint64_t bit = (uint64_t)frame / PAGE_SIZE;
    if (bitmap_test(bit)) {
        bitmap_clear(bit);
        used_frames--;
    }
}

uint64_t pmm_total_frames(void) { return total_frames; }
uint64_t pmm_free_frames(void)  { return total_frames - used_frames; }
