#ifndef NUGGET_DHCP_H
#define NUGGET_DHCP_H
#include "types.h"

/* Runs a full DHCP DISCOVER->OFFER->REQUEST->ACK handshake (bounded,
   blocking - like ARP/DNS resolution elsewhere in this stack). On success,
   fills in the offered IP/netmask/gateway and returns true. On any
   timeout/failure, returns false so the caller can fall back to a static
   IP instead. */
bool dhcp_negotiate(uint32_t *out_ip, uint32_t *out_netmask, uint32_t *out_gateway);

/* Called by the UDP layer when a reply arrives on port 68. */
void dhcp_handle_response(const uint8_t *data, uint16_t len);

#endif
