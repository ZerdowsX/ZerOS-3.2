#include "dns.h"
#include "udp.h"
#include "net.h"
#include "timer.h"
#include "string.h"
#include "serial.h"

/* Google Public DNS - reachable through the same NAT path everything else
   uses; no config UI for this yet, so it's just hardcoded for now. */
/* Both QEMU's usermode networking and VirtualBox's NAT engine provide a
   built-in DNS proxy at this address by convention (10.0.2.2 is the
   gateway, 10.0.2.3 is DNS) - using it instead of a public resolver like
   8.8.8.8 means we don't depend on the guest's outbound UDP:53 actually
   reaching the real internet, which some NAT setups don't allow even
   when normal TCP traffic works fine. */
#define DNS_SERVER_IP MAKE_IP(10, 0, 2, 3)
#define DNS_CLIENT_PORT 50053

static bool     pending = false;
static uint16_t query_id = 0;
static bool     got_response = false;
static uint32_t result_ip = 0;

static uint16_t read_be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

void dns_handle_response(const uint8_t *data, uint16_t len) {
    if (!pending || len < 12) return;

    uint16_t id = read_be16(data);
    if (id != query_id) return;

    uint16_t ancount = read_be16(data + 6);
    if (ancount == 0) return;

    /* Skip the question section: a sequence of length-prefixed labels
       ending in a zero byte, then QTYPE (2) + QCLASS (2). */
    uint16_t pos = 12;
    while (pos < len && data[pos] != 0) pos += (uint16_t)(data[pos] + 1);
    pos += 1 + 4;

    for (uint16_t i = 0; i < ancount && pos + 10 <= len; i++) {
        /* NAME: either a compression pointer (top two bits set, 2 bytes)
           or another length-prefixed label sequence. */
        if ((data[pos] & 0xC0) == 0xC0) {
            pos += 2;
        } else {
            while (pos < len && data[pos] != 0) pos += (uint16_t)(data[pos] + 1);
            pos += 1;
        }
        if (pos + 10 > len) break;

        uint16_t rtype = read_be16(data + pos); pos += 2;
        pos += 2; /* class */
        pos += 4; /* ttl */
        uint16_t rdlength = read_be16(data + pos); pos += 2;

        if (rtype == 1 && rdlength == 4 && pos + 4 <= len) { /* A record */
            result_ip = MAKE_IP(data[pos], data[pos+1], data[pos+2], data[pos+3]);
            got_response = true;
            return;
        }
        pos += rdlength;
    }
}

static int encode_qname(const char *hostname, uint8_t *out) {
    int out_i = 0;
    const char *label_start = hostname;
    while (1) {
        int label_len = 0;
        const char *p = label_start;
        while (*p && *p != '.') { label_len++; p++; }
        if (label_len > 63) label_len = 63; /* DNS label length limit */
        out[out_i++] = (uint8_t)label_len;
        memcpy(out + out_i, label_start, (size_t)label_len);
        out_i += label_len;
        if (*p == '\0') break;
        label_start = p + 1;
    }
    out[out_i++] = 0; /* root label */
    return out_i;
}

bool dns_resolve(const char *hostname, uint32_t *out_ip) {
    uint8_t packet[160];
    int i = 0;

    query_id = (uint16_t)timer_ticks();
    packet[i++] = (uint8_t)(query_id >> 8);
    packet[i++] = (uint8_t)(query_id & 0xFF);
    packet[i++] = 0x01; packet[i++] = 0x00; /* flags: standard query, recursion desired */
    packet[i++] = 0x00; packet[i++] = 0x01; /* QDCOUNT = 1 */
    packet[i++] = 0x00; packet[i++] = 0x00; /* ANCOUNT */
    packet[i++] = 0x00; packet[i++] = 0x00; /* NSCOUNT */
    packet[i++] = 0x00; packet[i++] = 0x00; /* ARCOUNT */

    i += encode_qname(hostname, packet + i);

    packet[i++] = 0x00; packet[i++] = 0x01; /* QTYPE = A */
    packet[i++] = 0x00; packet[i++] = 0x01; /* QCLASS = IN */

    pending = true;
    got_response = false;

    if (!net_resolve_arp_blocking(DNS_SERVER_IP, 2000)) {
        pending = false;
        serial_write("[dns] ARP resolution for resolver failed\n");
        return false;
    }

    if (!udp_send(DNS_SERVER_IP, 53, DNS_CLIENT_PORT, packet, (uint16_t)i)) {
        pending = false;
        return false;
    }

    uint64_t start = timer_ticks();
    while (timer_ticks() - start < 300) { /* 3 real seconds at 100 Hz */
        if (got_response) {
            *out_ip = result_ip;
            pending = false;
            return true;
        }
        __asm__ volatile ("sti; hlt");
    }

    pending = false;
    serial_write("[dns] resolve timed out\n");
    return false;
}
