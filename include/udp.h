#ifndef NUGGET_UDP_H
#define NUGGET_UDP_H
#include "types.h"

typedef struct PACKED {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;   /* header + data, big-endian */
    uint16_t checksum; /* 0 = not computed (allowed for IPv4 UDP) */
} udp_header_t;

bool udp_send(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port, const void *data, uint16_t len);

/* Sends a UDP datagram via broadcast, bypassing ARP - used for DHCP
   DISCOVER/REQUEST before we have a real IP. */
bool udp_send_broadcast(uint32_t src_ip, uint16_t dest_port, uint16_t src_port, const void *data, uint16_t len);

/* Called by the IP layer when a UDP datagram arrives for us. */
void udp_handle_packet(uint32_t src_ip, const uint8_t *packet, uint16_t len);

#endif
