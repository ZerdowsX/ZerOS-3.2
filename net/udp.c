#include "udp.h"
#include "net.h"
#include "string.h"

/* Forward declared here rather than pulling in dns.h/dhcp.h, to keep this
   file from needing full knowledge of every UDP consumer. */
extern void dns_handle_response(const uint8_t *data, uint16_t len);
extern void dhcp_handle_response(const uint8_t *data, uint16_t len);

bool udp_send(uint32_t dest_ip, uint16_t dest_port, uint16_t src_port, const void *data, uint16_t len) {
    uint8_t packet[600];
    if (len > sizeof(packet) - sizeof(udp_header_t)) return false;

    udp_header_t *udp = (udp_header_t*)packet;
    udp->src_port = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->length = htons((uint16_t)(sizeof(udp_header_t) + len));
    udp->checksum = 0; /* optional for IPv4 UDP - skip computing it */

    memcpy(packet + sizeof(udp_header_t), data, len);

    return ip_send(dest_ip, IP_PROTO_UDP, packet, (uint16_t)(sizeof(udp_header_t) + len));
}

bool udp_send_broadcast(uint32_t src_ip, uint16_t dest_port, uint16_t src_port, const void *data, uint16_t len) {
    uint8_t packet[600];
    if (len > sizeof(packet) - sizeof(udp_header_t)) return false;

    udp_header_t *udp = (udp_header_t*)packet;
    udp->src_port = htons(src_port);
    udp->dest_port = htons(dest_port);
    udp->length = htons((uint16_t)(sizeof(udp_header_t) + len));
    udp->checksum = 0;

    memcpy(packet + sizeof(udp_header_t), data, len);

    return ip_send_broadcast(IP_PROTO_UDP, src_ip, packet, (uint16_t)(sizeof(udp_header_t) + len));
}

void udp_handle_packet(uint32_t src_ip, const uint8_t *packet, uint16_t len) {
    (void)src_ip;
    if (len < sizeof(udp_header_t)) return;
    const udp_header_t *udp = (const udp_header_t*)packet;
    uint16_t src_port = ntohs(udp->src_port);
    uint16_t dest_port = ntohs(udp->dest_port);
    const uint8_t *payload = packet + sizeof(udp_header_t);
    uint16_t payload_len = (uint16_t)(len - sizeof(udp_header_t));

    if (src_port == 53) { /* DNS response */
        dns_handle_response(payload, payload_len);
    } else if (dest_port == 68) { /* DHCP reply (OFFER/ACK) */
        dhcp_handle_response(payload, payload_len);
    }
}
