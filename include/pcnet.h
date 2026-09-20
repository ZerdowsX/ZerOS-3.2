#ifndef NUGGET_PCNET_H
#define NUGGET_PCNET_H
#include "types.h"

bool pcnet_init(void);
void pcnet_get_mac(uint8_t mac[6]);
bool pcnet_send(const void *data, uint16_t len);
bool pcnet_is_ready(void);

typedef void (*pcnet_rx_callback_t)(const uint8_t *frame, uint16_t len);
void pcnet_set_rx_callback(pcnet_rx_callback_t cb);

#endif
