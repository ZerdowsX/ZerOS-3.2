#include "types.h"
#include "rtc.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool rtc_update_in_progress(void) {
    return cmos_read(0x0A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t val) {
    return (uint8_t)((val & 0x0F) + ((val >> 4) * 10));
}

void rtc_read_time(uint8_t *hours, uint8_t *minutes, uint8_t *seconds) {
    /* Wait out any in-progress update to avoid reading a half-updated value */
    int timeout = 100000;
    while (rtc_update_in_progress() && timeout--);

    uint8_t sec = cmos_read(0x00);
    uint8_t min = cmos_read(0x02);
    uint8_t hour = cmos_read(0x04);
    uint8_t status_b = cmos_read(0x0B);

    if (!(status_b & 0x04)) { /* bit 2 clear = values are in BCD, not binary */
        sec = bcd_to_bin(sec);
        min = bcd_to_bin(min);
        hour = bcd_to_bin(hour & 0x7F);
    }
    if (!(status_b & 0x02) && (hour & 0x80)) { /* 12-hour mode, PM bit set */
        hour = (uint8_t)(((hour & 0x7F) + 12) % 24);
    }

    *seconds = sec;
    *minutes = min;
    *hours = hour;
}
