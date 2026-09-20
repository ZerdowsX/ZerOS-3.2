#include "tcp.h"
#include "net.h"
#include "string.h"
#include "serial.h"
#include "vga.h"
#include "timer.h"

typedef struct {
    bool     in_use;
    uint16_t port;
} tcp_listener_t;

#define TCP_MAX_LISTENERS 8
static tcp_listener_t listeners[TCP_MAX_LISTENERS];
static tcp_connection_t connections[TCP_MAX_CONNECTIONS];

typedef struct PACKED {
    uint32_t src_ip;
    uint32_t dest_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_length;
} tcp_pseudo_header_t;

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *tcp_seg, uint16_t len) {
    uint8_t buf[1500 + sizeof(tcp_pseudo_header_t)];
    tcp_pseudo_header_t *ph = (tcp_pseudo_header_t*)buf;
    ph->src_ip = src_ip;
    ph->dest_ip = dst_ip;
    ph->zero = 0;
    ph->protocol = IP_PROTO_TCP;
    ph->tcp_length = htons(len);
    memcpy(buf + sizeof(tcp_pseudo_header_t), tcp_seg, len);
    return ip_checksum(buf, (uint32_t)(sizeof(tcp_pseudo_header_t) + len));
}

static tcp_connection_t *find_connection(uint32_t ip, uint16_t remote_port, uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (connections[i].in_use &&
            connections[i].remote_ip == ip &&
            connections[i].remote_port == remote_port &&
            connections[i].local_port == local_port) {
            return &connections[i];
        }
    }
    return NULL;
}

static tcp_connection_t *alloc_connection(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (!connections[i].in_use) {
            memset(&connections[i], 0, sizeof(tcp_connection_t));
            connections[i].in_use = true;
            return &connections[i];
        }
    }
    return NULL;
}

static uint16_t next_ephemeral_port = 49152;
static uint16_t alloc_ephemeral_port(void) {
    uint16_t port = next_ephemeral_port++;
    if (next_ephemeral_port == 0) next_ephemeral_port = 49152; /* wrap, avoid 0 */
    return port;
}

static bool is_listening(uint16_t port) {
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (listeners[i].in_use && listeners[i].port == port) return true;
    }
    return false;
}

bool tcp_listen(uint16_t port) {
    if (is_listening(port)) return true;
    for (int i = 0; i < TCP_MAX_LISTENERS; i++) {
        if (!listeners[i].in_use) {
            listeners[i].in_use = true;
            listeners[i].port = port;
            serial_write("[tcp] listening on port\n");
            return true;
        }
    }
    return false;
}

static void send_segment_raw(tcp_connection_t *conn, uint8_t flags, uint32_t seq, const void *data, uint16_t data_len) {
    uint8_t buf[1500];
    tcp_header_t *tcp = (tcp_header_t*)buf;
    tcp->src_port = htons(conn->local_port);
    tcp->dest_port = htons(conn->remote_port);
    tcp->seq_num = htonl(seq);
    tcp->ack_num = htonl(conn->recv_next);
    tcp->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags = flags;
    tcp->window = htons(TCP_RX_BUFFER_SIZE);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    if (data_len) memcpy(buf + sizeof(tcp_header_t), data, data_len);

    uint16_t total_len = (uint16_t)(sizeof(tcp_header_t) + data_len);
    tcp->checksum = tcp_checksum(net_get_ip(), conn->remote_ip, buf, total_len);

    ip_send(conn->remote_ip, IP_PROTO_TCP, buf, total_len);
}

static void send_segment(tcp_connection_t *conn, uint8_t flags, const void *data, uint16_t data_len) {
    send_segment_raw(conn, flags, conn->send_next, data, data_len);

    /* Remember this segment so tcp_poll() can resend it if no ACK shows up -
       only segments that actually need one (carry data, or SYN/FIN). */
    if ((flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) || data_len > 0) {
        conn->has_unacked_segment = true;
        conn->last_sent_flags = flags;
        conn->last_sent_seq = conn->send_next;
        conn->last_sent_len = (data_len < sizeof(conn->last_sent_data)) ? data_len : (uint16_t)sizeof(conn->last_sent_data);
        if (data_len) memcpy(conn->last_sent_data, data, conn->last_sent_len);
        conn->last_sent_time = timer_ticks();
        conn->retransmit_count = 0;
    }

    if (flags & (TCP_FLAG_SYN | TCP_FLAG_FIN)) {
        conn->send_next += 1; /* SYN/FIN each consume one sequence number */
    } else {
        conn->send_next += data_len;
    }
}

void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *segment, uint16_t len) {
    (void)dst_ip;
    if (len < sizeof(tcp_header_t)) return;
    const tcp_header_t *tcp = (const tcp_header_t*)segment;

    uint16_t src_port = ntohs(tcp->src_port);   /* remote port */
    uint16_t dest_port = ntohs(tcp->dest_port); /* our local port */
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t hdr_len = (uint8_t)((tcp->data_offset >> 4) * 4);
    const uint8_t *payload = segment + hdr_len;
    uint16_t payload_len = (uint16_t)(len - hdr_len);

    tcp_connection_t *conn = find_connection(src_ip, src_port, dest_port);

    if (!conn) {
        if ((tcp->flags & TCP_FLAG_SYN) && is_listening(dest_port)) {
            conn = alloc_connection();
            if (!conn) return; /* out of connection slots */
            conn->state = TCP_SYN_RECEIVED;
            conn->remote_ip = src_ip;
            conn->remote_port = src_port;
            conn->local_port = dest_port;
            conn->recv_next = seq + 1;
            conn->send_next = 0x1000; /* arbitrary initial sequence number */
            send_segment(conn, TCP_FLAG_SYN | TCP_FLAG_ACK, NULL, 0);
        }
        return; /* unsolicited segment for unknown connection: ignore (v1 - no RST) */
    }

    switch (conn->state) {
        case TCP_SYN_SENT:
            if ((tcp->flags & TCP_FLAG_SYN) && (tcp->flags & TCP_FLAG_ACK)) {
                conn->recv_next = seq + 1;
                conn->send_unacked = ack;
                conn->has_unacked_segment = false;
                conn->state = TCP_ESTABLISHED;
                send_segment(conn, TCP_FLAG_ACK, NULL, 0);
                serial_write("[tcp] outgoing connection established\n");
            }
            break;

        case TCP_SYN_RECEIVED:
            if (tcp->flags & TCP_FLAG_ACK) {
                conn->state = TCP_ESTABLISHED;
                serial_write("[tcp] connection established\n");
                send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, "Hello from Nugget OS!\n", 23);
            }
            break;

        case TCP_ESTABLISHED:
            if (payload_len > 0 && seq == conn->recv_next) {
                for (uint16_t i = 0; i < payload_len; i++) {
                    uint32_t next = (conn->rx_head + 1) % TCP_RX_BUFFER_SIZE;
                    if (next == conn->rx_tail) break; /* buffer full, drop rest */
                    conn->rx_buf[conn->rx_head] = payload[i];
                    conn->rx_head = next;
                }
                conn->recv_next += payload_len;
                send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            if (tcp->flags & TCP_FLAG_FIN) {
                conn->recv_next += 1;
                conn->state = TCP_CLOSE_WAIT;
                send_segment(conn, TCP_FLAG_ACK, NULL, 0);
            }
            if (tcp->flags & TCP_FLAG_ACK) {
                if (ack > conn->send_unacked) conn->send_unacked = ack;
                if (conn->has_unacked_segment && ack >= conn->send_next) {
                    conn->has_unacked_segment = false;
                }
            }
            break;

        case TCP_LAST_ACK:
            if (tcp->flags & TCP_FLAG_ACK) {
                conn->state = TCP_CLOSED;
                conn->in_use = false;
            }
            break;

        default:
            break;
    }
}

tcp_connection_t *tcp_connect(uint32_t dest_ip, uint16_t dest_port) {
    tcp_connection_t *conn = alloc_connection();
    if (!conn) return NULL;

    /* Resolve the peer's MAC first - otherwise the SYN we're about to send
       would silently vanish (ip_send drops the packet while ARP is pending). */
    if (!net_resolve_arp_blocking(dest_ip, 2000)) {
        conn->in_use = false;
        return NULL;
    }

    conn->state = TCP_SYN_SENT;
    conn->remote_ip = dest_ip;
    conn->remote_port = dest_port;
    conn->local_port = alloc_ephemeral_port();
    conn->send_next = 0x4000; /* arbitrary initial sequence number */
    conn->recv_next = 0;

    send_segment(conn, TCP_FLAG_SYN, NULL, 0);
    return conn;
}

bool tcp_is_established(tcp_connection_t *conn) {
    return conn && conn->in_use && conn->state == TCP_ESTABLISHED;
}

bool tcp_is_closed(tcp_connection_t *conn) {
    return !conn || !conn->in_use || conn->state == TCP_CLOSED;
}

bool tcp_is_peer_closed(tcp_connection_t *conn) {
    return conn && conn->in_use && conn->state == TCP_CLOSE_WAIT;
}

tcp_connection_t *tcp_accept(uint16_t port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (connections[i].in_use &&
            connections[i].local_port == port &&
            connections[i].state == TCP_ESTABLISHED) {
            return &connections[i];
        }
    }
    return NULL;
}

int tcp_send(tcp_connection_t *conn, const void *data, uint16_t len) {
    if (!conn || conn->state != TCP_ESTABLISHED) return -1;
    send_segment(conn, TCP_FLAG_ACK | TCP_FLAG_PSH, data, len);
    return len;
}

int tcp_recv(tcp_connection_t *conn, void *buf, uint16_t maxlen) {
    if (!conn) return -1;
    uint8_t *dst = (uint8_t*)buf;
    uint16_t count = 0;
    while (conn->rx_tail != conn->rx_head && count < maxlen) {
        dst[count++] = conn->rx_buf[conn->rx_tail];
        conn->rx_tail = (conn->rx_tail + 1) % TCP_RX_BUFFER_SIZE;
    }
    return count;
}

void tcp_close(tcp_connection_t *conn) {
    if (!conn) return;
    send_segment(conn, TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    conn->state = TCP_LAST_ACK;
}

#define TCP_RETRANSMIT_TIMEOUT_TICKS 300 /* 3 real seconds at our 100 Hz timer */
#define TCP_MAX_RETRANSMITS 5

void tcp_poll(void) {
    uint64_t now = timer_ticks();
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        tcp_connection_t *conn = &connections[i];
        if (!conn->in_use || !conn->has_unacked_segment) continue;

        if (now - conn->last_sent_time < TCP_RETRANSMIT_TIMEOUT_TICKS) continue;

        if (conn->retransmit_count >= TCP_MAX_RETRANSMITS) {
            serial_write("[tcp] giving up after repeated retransmit timeouts\n");
            conn->state = TCP_CLOSED;
            conn->in_use = false;
            continue;
        }

        /* Resend the exact same segment (same sequence number, same data) -
           the peer either never got it, or its ACK got lost; either way,
           trying again is the right move. Uses the raw sender directly so
           this doesn't disturb send_next or reset the retry counter. */
        send_segment_raw(conn, conn->last_sent_flags, conn->last_sent_seq, conn->last_sent_data, conn->last_sent_len);
        conn->retransmit_count++;
        conn->last_sent_time = now;
        serial_write("[tcp] retransmitting unacked segment\n");
    }
}
