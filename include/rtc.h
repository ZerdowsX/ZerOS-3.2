#ifndef NUGGET_RTC_H
#define NUGGET_RTC_H
#include "types.h"

/* Reads the current wall-clock time from the CMOS RTC (24-hour format). */
void rtc_read_time(uint8_t *hours, uint8_t *minutes, uint8_t *seconds);

#endif
