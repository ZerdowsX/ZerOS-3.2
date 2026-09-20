#ifndef NUGGET_DNS_H
#define NUGGET_DNS_H
#include "types.h"

/* Resolves a hostname to an IPv4 address via a public DNS resolver.
   Blocking (bounded poll, interrupts stay on), like ARP resolution.
   Returns false on timeout or malformed/negative response. */
bool dns_resolve(const char *hostname, uint32_t *out_ip);

/* Called by the UDP layer when a datagram from port 53 arrives. */
void dns_handle_response(const uint8_t *data, uint16_t len);

#endif
