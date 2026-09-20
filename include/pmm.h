#ifndef NUGGET_PMM_H
#define NUGGET_PMM_H
#include "types.h"

#define PAGE_SIZE 4096

void pmm_init(uint32_t mb2_info_addr, uint64_t kernel_end);
void *pmm_alloc_frame(void);
void pmm_free_frame(void *frame);
uint64_t pmm_total_frames(void);
uint64_t pmm_free_frames(void);

#endif
