#ifndef NUGGET_HEAP_H
#define NUGGET_HEAP_H
#include "types.h"

void heap_init(uint64_t start, uint64_t size);
void *kmalloc(size_t size);
void kfree(void *ptr);

#endif
