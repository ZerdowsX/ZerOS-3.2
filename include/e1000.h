#ifndef NUGGET_E1000_H
#define NUGGET_E1000_H
#include "types.h"

bool e1000_init(void);
void e1000_get_mac(uint8_t out[6]);
bool e1000_send(const void *data, uint16_t len);
bool e1000_is_ready(void);

typedef void (*e1000_rx_callback_t)(const uint8_t *frame, uint16_t len);
void e1000_set_rx_callback(e1000_rx_callback_t cb);

#endif
