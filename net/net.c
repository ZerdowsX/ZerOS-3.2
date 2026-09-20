#include "net.h"
#include "udp.h"
#include "string.h"
#include "serial.h"
#include "vga.h"
#include "timer.h"

static uint32_t my_ip = 0;
static uint32_t my_netmask = 0;
static uint32_t my_gateway = 0;
static uint8_t  my_mac[6];
static nic_send_fn nic_send = NULL;
static nic_get_mac_fn nic_get_mac = NULL;

void net_set_nic(nic_send_fn send_fn, nic_get_mac_fn mac_fn) {
    nic_send = send_fn;
    nic_get_mac = mac_fn;
}

#define ARP_CACHE_SIZE 16
typedef struct { uint32_t ip; uint8_t mac[6]; bool valid; } arp_entry_t;
static arp_entry_t arp_cache[ARP_CACHE_SIZE];

void net_set_ip_config(uint32_t ip, uint32_t netmask, uint32_t gateway) {
    my_ip = ip;
    my_netmask = netmask;
    my_gateway = gateway;
    if (nic_get_mac) nic_get_mac(my_mac);
}

uint32_t net_get_ip(void) { return my_ip; }
void net_get_mac(uint8_t out[6]) { memcpy(out, my_mac, 6); }

uint16_t ip_checksum(const void *data, uint32_t len) {
    const uint16_t *ptr = (const uint16_t*)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }
    if (len == 1) sum += *(const uint8_t*)ptr;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static void arp_cache_insert(uint32_t ip, const uint8_t *mac) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    }
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, mac, 6);
            arp_cache[i].valid = true;
            return;
        }
    }
    /* cache full - overwrite slot 0 (simple policy, fine for a hobby OS) */
    arp_cache[0].ip = ip;
    memcpy(arp_cache[0].mac, mac, 6);
}

static bool arp_cache_lookup(uint32_t ip, uint8_t *mac_out) {
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            memcpy(mac_out, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

static void eth_send(const uint8_t *dest_mac, uint16_t ethertype, const void *payload, uint16_t len) {
    uint8_t frame[1514];
    eth_header_t *eth = (eth_header_t*)frame;
    memcpy(eth->dest_mac, dest_mac, 6);
    memcpy(eth->src_mac, my_mac, 6);
    eth->ethertype = htons(ethertype);
    memcpy(frame + sizeof(eth_header_t), payload, len);
    if (nic_send) nic_send(frame, sizeof(eth_header_t) + len);
}

bool ip_send_broadcast(uint8_t protocol, uint32_t src_ip, const void *payload, uint16_t len) {
    uint8_t packet[1500];
    ip_header_t *ip = (ip_header_t*)packet;
    ip->version_ihl = 0x45;
    ip->dscp_ecn = 0;
    ip->total_length = htons((uint16_t)(sizeof(ip_header_t) + len));
    ip->id = htons(0);
    ip->flags_fragment = htons(0x4000);
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_ip = src_ip;
    ip->dest_ip = 0xFFFFFFFF; /* 255.255.255.255 */
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

    memcpy(packet + sizeof(ip_header_t), payload, len);

    uint8_t broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eth_send(broadcast_mac, ETH_TYPE_IPV4, packet, (uint16_t)(sizeof(ip_header_t) + len));
    return true;
}

void arp_send_request(uint32_t target_ip) {
    arp_packet_t arp;
    arp.htype = htons(ARP_HTYPE_ETHERNET);
    arp.ptype = htons(ETH_TYPE_IPV4);
    arp.hlen = 6;
    arp.plen = 4;
    arp.oper = htons(ARP_OP_REQUEST);
    memcpy(arp.sender_mac, my_mac, 6);
    arp.sender_ip = my_ip;
    memset(arp.target_mac, 0, 6);
    arp.target_ip = target_ip;

    uint8_t broadcast[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
    eth_send(broadcast, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static void arp_handle(const arp_packet_t *arp) {
    arp_cache_insert(arp->sender_ip, arp->sender_mac);

    if (ntohs(arp->oper) == ARP_OP_REQUEST && arp->target_ip == my_ip) {
        arp_packet_t reply;
        reply.htype = htons(ARP_HTYPE_ETHERNET);
        reply.ptype = htons(ETH_TYPE_IPV4);
        reply.hlen = 6;
        reply.plen = 4;
        reply.oper = htons(ARP_OP_REPLY);
        memcpy(reply.sender_mac, my_mac, 6);
        reply.sender_ip = my_ip;
        memcpy(reply.target_mac, arp->sender_mac, 6);
        reply.target_ip = arp->sender_ip;
        eth_send(arp->sender_mac, ETH_TYPE_ARP, &reply, sizeof(reply));
    }
}

/* Forward declaration - implemented in tcp.c, called for TCP segments */
extern void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *segment, uint16_t len);

static void ip_handle(const uint8_t *packet, uint16_t len) {
    const ip_header_t *ip = (const ip_header_t*)packet;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    if (ihl < sizeof(ip_header_t)) return;

    if (ip->dest_ip != my_ip && ip->dest_ip != 0xFFFFFFFF) return; /* not for us / not broadcast */

    const uint8_t *payload = packet + ihl;
    uint16_t payload_len = ntohs(ip->total_length) - ihl;

    if (ip->protocol == IP_PROTO_TCP) {
        tcp_handle_packet(ip->src_ip, ip->dest_ip, payload, payload_len);
    } else if (ip->protocol == IP_PROTO_UDP) {
        udp_handle_packet(ip->src_ip, payload, payload_len);
    }
    /* ICMP: future work */
    (void)len;
}

bool ip_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t len) {
    uint8_t packet[1500];
    ip_header_t *ip = (ip_header_t*)packet;
    ip->version_ihl = 0x45; /* version 4, IHL 5 (20 bytes, no options) */
    ip->dscp_ecn = 0;
    ip->total_length = htons((uint16_t)(sizeof(ip_header_t) + len));
    ip->id = htons(0);
    ip->flags_fragment = htons(0x4000); /* don't fragment */
    ip->ttl = 64;
    ip->protocol = protocol;
    ip->checksum = 0;
    ip->src_ip = my_ip;
    ip->dest_ip = dest_ip;
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

    memcpy(packet + sizeof(ip_header_t), payload, len);

    /* Determine next-hop MAC: direct if on-link, else via gateway */
    uint32_t next_hop = ((dest_ip & my_netmask) == (my_ip & my_netmask)) ? dest_ip : my_gateway;

    uint8_t mac[6];
    if (!arp_cache_lookup(next_hop, mac)) {
        arp_send_request(next_hop);
        return false; /* caller should retry shortly after ARP resolves */
    }

    eth_send(mac, ETH_TYPE_IPV4, packet, (uint16_t)(sizeof(ip_header_t) + len));
    return true;
}

void net_rx_handler(const uint8_t *frame, uint16_t len) {
    if (len < sizeof(eth_header_t)) return;
    const eth_header_t *eth = (const eth_header_t*)frame;
    uint16_t ethertype = ntohs(eth->ethertype);
    const uint8_t *payload = frame + sizeof(eth_header_t);
    uint16_t payload_len = (uint16_t)(len - sizeof(eth_header_t));

    if (ethertype == ETH_TYPE_ARP) {
        if (payload_len >= sizeof(arp_packet_t)) arp_handle((const arp_packet_t*)payload);
    } else if (ethertype == ETH_TYPE_IPV4) {
        ip_handle(payload, payload_len);
    }
}

void net_init(void) {
    memset(arp_cache, 0, sizeof(arp_cache));
    serial_write("[net] ethernet/arp/ip layer initialized\n");
}

bool net_resolve_arp_blocking(uint32_t ip, uint32_t timeout_ms) {
    uint32_t next_hop = ((ip & my_netmask) == (my_ip & my_netmask)) ? ip : my_gateway;

    uint8_t mac[6];
    if (arp_cache_lookup(next_hop, mac)) return true;

    arp_send_request(next_hop);

    uint64_t start = timer_ticks();
    uint64_t timeout_ticks = (timeout_ms * 100) / 1000; /* timer runs at 100 Hz */
    if (timeout_ticks == 0) timeout_ticks = 1;

    while (timer_ticks() - start < timeout_ticks) {
        if (arp_cache_lookup(next_hop, mac)) return true;
        __asm__ volatile ("sti; hlt");
    }
    return false;
}

/* Exposed so kernel_main can wire up whichever NIC driver actually found a card. */
void net_rx_handler_entry(const uint8_t *frame, uint16_t len) {
    net_rx_handler(frame, len);
}
