#ifndef NUGGET_TCP_H
#define NUGGET_TCP_H
#include "types.h"

typedef struct PACKED {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;   /* upper 4 bits = header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK
} tcp_state_t;

#define TCP_MAX_CONNECTIONS 16
#define TCP_RX_BUFFER_SIZE  4096

typedef struct {
    bool        in_use;
    tcp_state_t state;
    uint32_t    remote_ip;
    uint16_t    remote_port;
    uint16_t    local_port;
    uint32_t    send_next;      /* next sequence number we will send */
    uint32_t    send_unacked;   /* oldest unacknowledged sequence number */
    uint32_t    recv_next;      /* next sequence number we expect from peer */

    uint8_t     rx_buf[TCP_RX_BUFFER_SIZE];
    uint32_t    rx_head, rx_tail; /* ring buffer indices for received app data */

    /* Retransmission: remembers the last segment that still needs an ACK,
       so we can resend it if the peer doesn't answer in time. */
    bool        has_unacked_segment;
    uint8_t     last_sent_data[512];
    uint16_t    last_sent_len;
    uint8_t     last_sent_flags;
    uint32_t    last_sent_seq;
    uint64_t    last_sent_time;
    int         retransmit_count;
} tcp_connection_t;

/* Puts a port into LISTEN state - accepted connections show up via tcp_accept(). */
bool tcp_listen(uint16_t port);

/* Actively opens an outgoing connection (client-side) - used by ZerBrowser.
   Returns a handle immediately in SYN_SENT state; poll tcp_is_established()
   or tcp_is_closed() on it to know when the handshake finishes (or fails). */
tcp_connection_t *tcp_connect(uint32_t dest_ip, uint16_t dest_port);
bool tcp_is_established(tcp_connection_t *conn);
bool tcp_is_closed(tcp_connection_t *conn);
bool tcp_is_peer_closed(tcp_connection_t *conn); /* remote sent FIN - no more data coming */

/* Returns an established connection that hasn't been handed to the app yet, or NULL. */
tcp_connection_t *tcp_accept(uint16_t port);

int  tcp_send(tcp_connection_t *conn, const void *data, uint16_t len);
int  tcp_recv(tcp_connection_t *conn, void *buf, uint16_t maxlen);
void tcp_close(tcp_connection_t *conn);

void tcp_handle_packet(uint32_t src_ip, uint32_t dst_ip, const uint8_t *segment, uint16_t len);
void tcp_poll(void); /* call periodically from the main loop to drive retransmits/timeouts */

#endif
