#ifndef NUGGET_TIMER_H
#define NUGGET_TIMER_H
#include "types.h"

void timer_init(uint32_t hz);
uint64_t timer_ticks(void);
uint64_t timer_uptime_ms(void);
void timer_sleep_ms(uint64_t ms);

#endif
