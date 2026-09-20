#ifndef NUGGET_NET_H
#define NUGGET_NET_H
#include "types.h"

static inline uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
static inline uint16_t ntohs(uint16_t v) { return htons(v); }
static inline uint32_t htonl(uint32_t v) {
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | ((v >> 24) & 0xFF);
}
static inline uint32_t ntohl(uint32_t v) { return htonl(v); }

#define ETH_TYPE_ARP  0x0806
#define ETH_TYPE_IPV4 0x0800

typedef struct PACKED {
    uint8_t  dest_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype; /* big-endian */
    /* payload follows */
} eth_header_t;

#define ARP_HTYPE_ETHERNET 1
#define ARP_OP_REQUEST     1
#define ARP_OP_REPLY       2

typedef struct PACKED {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
} arp_packet_t;

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

typedef struct PACKED {
    uint8_t  version_ihl;   /* version(4 bits) + IHL(4 bits) */
    uint8_t  dscp_ecn;
    uint16_t total_length;
    uint16_t id;
    uint16_t flags_fragment;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dest_ip;
} ip_header_t;

uint16_t ip_checksum(const void *data, uint32_t len);

/* Global config (set once we get an address, static for v1 - DHCP is future work) */
void net_set_ip_config(uint32_t ip, uint32_t netmask, uint32_t gateway);
uint32_t net_get_ip(void);
void net_get_mac(uint8_t out[6]);
void net_init(void);
void arp_send_request(uint32_t target_ip);
bool ip_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len);
/* Sends via the Ethernet/IP broadcast address, bypassing ARP - used by DHCP
   DISCOVER/REQUEST, which happen before we have a real IP or a resolvable
   gateway to ARP against. Caller supplies the source IP explicitly (usually
   0.0.0.0 during negotiation). */
bool ip_send_broadcast(uint8_t protocol, uint32_t src_ip, const void *payload, uint16_t len);

/* Blocks (polling, interrupts stay enabled so RX still runs) until the given
   IP's MAC is known via ARP, or timeout_ms elapses. Needed before actively
   opening an outgoing TCP connection, since the very first packet we send
   (the SYN) would otherwise silently vanish if the peer's MAC isn't cached. */
bool net_resolve_arp_blocking(uint32_t ip, uint32_t timeout_ms);

/* NIC abstraction - lets net.c work with whichever driver actually found a
   card (RTL8139 in QEMU, PCNet in VirtualBox) without hardcoding either. */
typedef bool (*nic_send_fn)(const void *data, uint16_t len);
typedef void (*nic_get_mac_fn)(uint8_t mac[6]);
void net_set_nic(nic_send_fn send_fn, nic_get_mac_fn mac_fn);
void net_rx_handler_entry(const uint8_t *frame, uint16_t len);

#define MAKE_IP(a,b,c,d) ( ((uint32_t)(a)) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24) )

#endif
