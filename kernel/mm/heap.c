#include "heap.h"
#include "serial.h"

/* Simple first-fit free-list allocator over a static identity-mapped region. */

typedef struct block_header {
    size_t size;              /* size of usable memory following this header */
    uint8_t free;
    struct block_header *next;
} block_header_t;

static block_header_t *heap_start = NULL;
static uint64_t heap_end = 0;
static uint64_t heap_cursor = 0;

#define ALIGN8(x) (((x) + 7) & ~((size_t)7))
#define HEADER_SIZE sizeof(block_header_t)

void heap_init(uint64_t start, uint64_t size) {
    heap_cursor = start;
    heap_end = start + size;
    heap_start = NULL;
    serial_write("[heap] initialized\n");
}

static block_header_t *request_block(size_t size) {
    size_t total = HEADER_SIZE + size;
    if (heap_cursor + total > heap_end) return NULL; /* out of heap space */
    block_header_t *blk = (block_header_t*)heap_cursor;
    heap_cursor += total;
    blk->size = size;
    blk->free = 0;
    blk->next = NULL;
    return blk;
}

void *kmalloc(size_t size) {
    size = ALIGN8(size);
    if (size == 0) size = 8;

    block_header_t *cur = heap_start;
    block_header_t *prev = NULL;

    while (cur) {
        if (cur->free && cur->size >= size) {
            cur->free = 0;
            return (void*)((uint8_t*)cur + HEADER_SIZE);
        }
        prev = cur;
        cur = cur->next;
    }

    block_header_t *blk = request_block(size);
    if (!blk) return NULL;

    if (prev) prev->next = blk;
    else heap_start = blk;

    return (void*)((uint8_t*)blk + HEADER_SIZE);
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_header_t *blk = (block_header_t*)((uint8_t*)ptr - HEADER_SIZE);
    blk->free = 1;

    /* Coalesce adjacent free blocks (simple, single pass, O(n)) */
    block_header_t *cur = heap_start;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += HEADER_SIZE + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }
}
