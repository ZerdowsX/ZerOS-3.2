#include "dhcp.h"
#include "udp.h"
#include "net.h"
#include "timer.h"
#include "string.h"
#include "serial.h"

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_OP_REQUEST 1
#define DHCP_HTYPE_ETHERNET 1

#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER    2
#define DHCP_MSG_REQUEST  3
#define DHCP_MSG_ACK      5
#define DHCP_MSG_NAK      6

typedef struct PACKED {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  magic[4];
    uint8_t  options[64];
} dhcp_packet_t;

static uint32_t dhcp_xid = 0x4E554747; /* "NUGG" - fixed, we only ever run one negotiation at a time */

static bool got_offer = false, got_ack = false, got_nak = false;
static uint32_t offered_ip = 0, offered_server_ip = 0, offered_mask = 0, offered_gateway = 0;

static uint16_t read_be16_dh(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t read_be32_dh(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

void dhcp_handle_response(const uint8_t *data, uint16_t len) {
    if (len < sizeof(dhcp_packet_t) - sizeof(((dhcp_packet_t*)0)->options)) return;
    const dhcp_packet_t *pkt = (const dhcp_packet_t*)data;
    if (pkt->xid != dhcp_xid) return; /* not a reply to our current negotiation */
    if (pkt->magic[0] != 99 || pkt->magic[1] != 130 || pkt->magic[2] != 83 || pkt->magic[3] != 99) return;

    uint32_t yiaddr = read_be32_dh((const uint8_t*)&pkt->yiaddr);
    uint32_t siaddr_field = read_be32_dh((const uint8_t*)&pkt->siaddr);

    uint8_t msg_type = 0;
    uint32_t mask = 0, gw = 0, server_id = siaddr_field;

    uint16_t opt_len = (uint16_t)(len - (uint16_t)((const uint8_t*)pkt->options - (const uint8_t*)pkt));
    const uint8_t *opt = pkt->options;
    uint16_t i = 0;
    while (i < opt_len) {
        uint8_t code = opt[i];
        if (code == 255) break;
        if (code == 0) { i++; continue; }
        if (i + 1 >= opt_len) break;
        uint8_t opt_l = opt[i + 1];
        if (i + 2 + opt_l > opt_len) break;
        const uint8_t *val = opt + i + 2;

        if (code == 53 && opt_l >= 1) msg_type = val[0];
        else if (code == 1 && opt_l >= 4) mask = read_be32_dh(val);
        else if (code == 3 && opt_l >= 4) gw = read_be32_dh(val);
        else if (code == 54 && opt_l >= 4) server_id = read_be32_dh(val);

        i += (uint16_t)(2 + opt_l);
    }

    if (msg_type == DHCP_MSG_OFFER) {
        offered_ip = yiaddr;
        offered_server_ip = server_id;
        offered_mask = mask;
        offered_gateway = gw;
        got_offer = true;
    } else if (msg_type == DHCP_MSG_ACK) {
        offered_ip = yiaddr;
        if (mask) offered_mask = mask;
        if (gw) offered_gateway = gw;
        got_ack = true;
    } else if (msg_type == DHCP_MSG_NAK) {
        got_nak = true;
    }
}

static int build_options_discover(uint8_t *opt) {
    int i = 0;
    opt[i++] = 53; opt[i++] = 1; opt[i++] = DHCP_MSG_DISCOVER;
    opt[i++] = 55; opt[i++] = 3; opt[i++] = 1; opt[i++] = 3; opt[i++] = 6; /* mask, router, DNS */
    opt[i++] = 255;
    return i;
}

static int build_options_request(uint8_t *opt, uint32_t req_ip, uint32_t server_id) {
    int i = 0;
    opt[i++] = 53; opt[i++] = 1; opt[i++] = DHCP_MSG_REQUEST;
    opt[i++] = 50; opt[i++] = 4;
    opt[i++] = (uint8_t)(req_ip >> 24); opt[i++] = (uint8_t)(req_ip >> 16);
    opt[i++] = (uint8_t)(req_ip >> 8);  opt[i++] = (uint8_t)req_ip;
    opt[i++] = 54; opt[i++] = 4;
    opt[i++] = (uint8_t)(server_id >> 24); opt[i++] = (uint8_t)(server_id >> 16);
    opt[i++] = (uint8_t)(server_id >> 8);  opt[i++] = (uint8_t)server_id;
    opt[i++] = 55; opt[i++] = 3; opt[i++] = 1; opt[i++] = 3; opt[i++] = 6;
    opt[i++] = 255;
    return i;
}

static void send_dhcp_packet(uint8_t msg_type_unused, int opt_len_precomputed, const uint8_t *opts) {
    (void)msg_type_unused;
    dhcp_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.op = DHCP_OP_REQUEST;
    pkt.htype = DHCP_HTYPE_ETHERNET;
    pkt.hlen = 6;
    pkt.xid = dhcp_xid; /* sent as-is; both sides just need to echo it back, byte order is irrelevant to us */
    pkt.flags = htons(0x8000); /* broadcast flag: ask the server to reply via broadcast, since we can't
                                   receive unicast to an IP we haven't configured on the NIC yet */
    net_get_mac(pkt.chaddr);
    pkt.magic[0] = 99; pkt.magic[1] = 130; pkt.magic[2] = 83; pkt.magic[3] = 99;
    memcpy(pkt.options, opts, (size_t)opt_len_precomputed);

    uint16_t total = (uint16_t)((uint8_t*)pkt.options - (uint8_t*)&pkt) + (uint16_t)opt_len_precomputed;
    udp_send_broadcast(0, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, &pkt, total);
}

bool dhcp_negotiate(uint32_t *out_ip, uint32_t *out_netmask, uint32_t *out_gateway) {
    got_offer = false; got_ack = false; got_nak = false;
    dhcp_xid = (uint32_t)timer_ticks() ^ 0x4E554747u;

    uint8_t opts[32];
    int olen = build_options_discover(opts);
    send_dhcp_packet(DHCP_MSG_DISCOVER, olen, opts);
    serial_write("[dhcp] sent DISCOVER\n");

    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 300 && !got_offer) { /* 3 real seconds */
        __asm__ volatile ("sti; hlt");
    }
    if (!got_offer) {
        serial_write("[dhcp] no OFFER received, giving up\n");
        return false;
    }
    serial_write("[dhcp] got OFFER\n");

    olen = build_options_request(opts, offered_ip, offered_server_ip);
    send_dhcp_packet(DHCP_MSG_REQUEST, olen, opts);

    start = timer_ticks();
    while (timer_ticks() - start < 300 && !got_ack && !got_nak) {
        __asm__ volatile ("sti; hlt");
    }
    if (got_nak || !got_ack) {
        serial_write("[dhcp] request not acknowledged, giving up\n");
        return false;
    }
    serial_write("[dhcp] got ACK\n");

    *out_ip = offered_ip;
    *out_netmask = offered_mask ? offered_mask : 0xFFFFFF00; /* default to /24 if the server didn't say */
    *out_gateway = offered_gateway;
    return true;
}
