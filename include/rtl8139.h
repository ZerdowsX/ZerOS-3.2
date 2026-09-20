#ifndef NUGGET_RTL8139_H
#define NUGGET_RTL8139_H
#include "types.h"

bool rtl8139_init(void);
void rtl8139_get_mac(uint8_t mac[6]);
bool rtl8139_send(const void *data, uint16_t len);
bool rtl8139_is_ready(void);

/* Registers a callback invoked from the RX interrupt handler with each
 * received Ethernet frame (called in interrupt context - keep it fast). */
typedef void (*rtl8139_rx_callback_t)(const uint8_t *frame, uint16_t len);
void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb);

#endif
