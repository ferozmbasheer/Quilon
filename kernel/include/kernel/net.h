/*
 * Quilon OS -- Minimal TCP/IP Stack (Section 10.3)
 *
 * Layer 2  Ethernet II  -- eth_hdr_t          14 bytes
 * Layer 3  ARP          -- arp_pkt_t          28 bytes (IPv4 / Ethernet)
 * Layer 3  IPv4         -- ipv4_hdr_t         20 bytes (no options)
 * Layer 4  ICMP         -- icmp_hdr_t          8 bytes (echo header only)
 * Layer 4  UDP          -- udp_hdr_t           8 bytes
 * Layer 4  TCP          -- tcp_hdr_t          20 bytes (no options)
 * Application  DHCP     -- dhcp_msg_t        300 bytes (minimum)
 *
 * All on-wire multi-byte fields are stored in network (big-endian) byte
 * order.  The pure-C helpers at the bottom of this header are
 * static inline so that host-side unit tests can include this file and
 * exercise them without any x86 I/O instructions.
 */

#ifndef _KERNEL_NET_H
#define _KERNEL_NET_H

#include <stdint.h>

/* -- EtherType values -------------------------------------------------- */
#define ETHERTYPE_IPV4   0x0800u
#define ETHERTYPE_ARP    0x0806u

/* -- IPv4 protocol numbers --------------------------------------------- */
#define IPPROTO_ICMP     1u
#define IPPROTO_TCP      6u
#define IPPROTO_UDP      17u

/* -- ICMP type codes --------------------------------------------------- */
#define ICMP_ECHO_REPLY   0u
#define ICMP_ECHO_REQUEST 8u

/* -- ARP operation codes ----------------------------------------------- */
#define ARP_OP_REQUEST   1u
#define ARP_OP_REPLY     2u

/* -- TCP flag bits (tcp_hdr_t.flags byte) ------------------------------ */
#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u
#define TCP_URG  0x20u

/* -- BSD socket API constants (section 15.1) --------------------------- */
#define AF_INET       2u   /* IPv4 address family            */
#define SOCK_STREAM   1u   /* reliable byte stream (TCP)     */
#define SOCK_DGRAM    2u   /* datagram (UDP)                 */

/* -- Well-known ports -------------------------------------------------- */
#define PORT_DHCP_SERVER  67u
#define PORT_DHCP_CLIENT  68u
#define PORT_HTTP         80u
#define PORT_ECHO          7u   /* UDP/TCP echo server (RFC 862) */

/* -- On-wire header sizes (bytes) -------------------------------------- */
#define ETH_HDR_SIZE      14u
#define ETH_MAC_LEN        6u
#define ARP_PKT_SIZE      28u   /* fixed for IPv4-over-Ethernet */
#define IPV4_HDR_SIZE     20u   /* no options */
#define ICMP_HDR_SIZE      8u
#define UDP_HDR_SIZE       8u
#define TCP_HDR_SIZE      20u   /* no options */
#define DHCP_FIXED_SIZE  236u   /* fixed fields before magic cookie */
#define DHCP_MIN_SIZE    300u   /* min DHCP payload (RFC 2131)     */

/* -- DHCP magic cookie ------------------------------------------------- */
#define DHCP_MAGIC_COOKIE  0x63825363u

/* -- DHCP message type codes ------------------------------------------- */
#define DHCP_MSG_DISCOVER  1u
#define DHCP_MSG_OFFER     2u
#define DHCP_MSG_REQUEST   3u
#define DHCP_MSG_ACK       5u

/* -- Stack limits ------------------------------------------------------ */
#define NET_ARP_CACHE_SIZE  4u    /* ARP cache entries              */
#define NET_UDP_MAX_PAYLOAD 512u  /* max UDP payload buffered in RX */
#define NET_TCP_RX_BUF      512u  /* TCP receive data buffer        */
#define NET_ICMP_ECHO_DATA   32u  /* bytes of payload in our pings  */

/* =======================================================================
 * On-wire structures (all fields in network / big-endian byte order)
 * ======================================================================= */

typedef struct {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype;   /* big-endian */
} __attribute__((packed)) eth_hdr_t;

typedef struct {
    uint16_t hw_type;     /* 0x0001 = Ethernet  */
    uint16_t proto_type;  /* 0x0800 = IPv4      */
    uint8_t  hw_len;      /* 6                  */
    uint8_t  proto_len;   /* 4                  */
    uint16_t operation;   /* 1=request 2=reply  */
    uint8_t  sender_mac[6];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[6];
    uint8_t  target_ip[4];
} __attribute__((packed)) arp_pkt_t;

typedef struct {
    uint8_t  ver_ihl;    /* version(4b) | IHL(4b); 0x45 = no options */
    uint8_t  dscp_ecn;
    uint16_t total_len;  /* header + payload */
    uint16_t ident;
    uint16_t frag_off;   /* flags + fragment offset */
    uint8_t  ttl;
    uint8_t  protocol;   /* IPPROTO_* */
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed)) ipv4_hdr_t;

typedef struct {
    uint8_t  type;       /* ICMP_ECHO_REQUEST / ICMP_ECHO_REPLY */
    uint8_t  code;       /* 0 for echo */
    uint16_t checksum;
    uint16_t ident;
    uint16_t seq;
    /* data follows */
} __attribute__((packed)) icmp_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;     /* header + payload */
    uint16_t checksum;   /* 0 = disabled for IPv4 */
} __attribute__((packed)) udp_hdr_t;

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t  data_offset;  /* (hdr_len / 4) << 4 */
    uint8_t  flags;        /* TCP_FIN|SYN|RST|PSH|ACK|URG */
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent;
} __attribute__((packed)) tcp_hdr_t;

/* DHCP message (RFC 2131).  options[64] holds the magic cookie and
 * option TLVs; total struct size = 236 + 64 = 300 bytes = DHCP_MIN_SIZE. */
typedef struct {
    uint8_t  op;          /* 1=BOOTREQUEST 2=BOOTREPLY */
    uint8_t  htype;       /* 1=Ethernet */
    uint8_t  hlen;        /* 6 */
    uint8_t  hops;
    uint32_t xid;         /* transaction ID */
    uint16_t secs;
    uint16_t flags;       /* 0x8000 = broadcast */
    uint8_t  ciaddr[4];   /* client IP (0.0.0.0 for DISCOVER) */
    uint8_t  yiaddr[4];   /* offered IP */
    uint8_t  siaddr[4];   /* server IP */
    uint8_t  giaddr[4];   /* relay agent IP */
    uint8_t  chaddr[16];  /* client hardware address (MAC + padding) */
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[64]; /* magic cookie + TLVs */
} __attribute__((packed)) dhcp_msg_t;

/* =======================================================================
 * Pure-C helpers -- testable on host (no x86 I/O)
 * ======================================================================= */

/* -- Byte-order (host = little-endian x86; network = big-endian) ------- */

static inline uint16_t net_htons(uint16_t v)
{
    return (uint16_t)((v >> 8) | (v << 8));
}
static inline uint16_t net_ntohs(uint16_t v) { return net_htons(v); }

static inline uint32_t net_htonl(uint32_t v)
{
    return ((v & 0x000000FFu) << 24)
         | ((v & 0x0000FF00u) <<  8)
         | ((v & 0x00FF0000u) >>  8)
         | ((v & 0xFF000000u) >> 24);
}
static inline uint32_t net_ntohl(uint32_t v) { return net_htonl(v); }

/* -- IP address helpers ------------------------------------------------ */

/* Read 4 network-byte-order bytes into a host-order uint32_t. */
static inline uint32_t net_ip_read(const uint8_t ip[4])
{
    return ((uint32_t)ip[0] << 24)
         | ((uint32_t)ip[1] << 16)
         | ((uint32_t)ip[2] <<  8)
         |  (uint32_t)ip[3];
}

/* Write a host-order uint32_t into 4 network-byte-order bytes. */
static inline void net_ip_write(uint8_t dst[4], uint32_t ip)
{
    dst[0] = (uint8_t)(ip >> 24);
    dst[1] = (uint8_t)(ip >> 16);
    dst[2] = (uint8_t)(ip >>  8);
    dst[3] = (uint8_t)(ip      );
}

/*
 * net_checksum16 -- RFC 1071 one's-complement 16-bit checksum.
 *
 * To compute a checksum: zero the checksum field, call this function,
 * and store the returned value in the checksum field.
 *
 * To verify: call with the complete header (checksum field set).
 * A valid header returns 0x0000.
 */
static inline uint16_t net_checksum16(const void *data, int len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    while (len >= 2) {
        sum += (uint32_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
        p   += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint32_t)p[0] << 8;   /* zero-pad odd byte */

    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);

    return (uint16_t)~sum;
}

/*
 * net_transport_checksum -- compute TCP or UDP pseudo-header checksum.
 *
 * Covers: 12-byte IPv4 pseudo-header (src_ip, dst_ip, 0, proto, seg_len)
 *         + the transport segment (header + data).
 *
 * Caller must zero the checksum field in the segment before calling.
 */
static inline uint16_t net_transport_checksum(uint32_t src_ip, uint32_t dst_ip,
                                               uint8_t  proto,
                                               const void *segment,
                                               uint16_t   seg_len)
{
    uint32_t sum = 0;
    uint8_t  ph[12];
    const uint8_t *p;
    int len;

    /* Build pseudo-header */
    net_ip_write(ph + 0, src_ip);
    net_ip_write(ph + 4, dst_ip);
    ph[8]  = 0;
    ph[9]  = proto;
    ph[10] = (uint8_t)(seg_len >> 8);
    ph[11] = (uint8_t)(seg_len);

    for (p = ph, len = 12; len >= 2; p += 2, len -= 2)
        sum += (uint32_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);

    for (p = (const uint8_t *)segment, len = seg_len; len >= 2; p += 2, len -= 2)
        sum += (uint32_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
    if (len == 1)
        sum += (uint32_t)p[0] << 8;

    while (sum >> 16)
        sum = (sum & 0xFFFFu) + (sum >> 16);

    return (uint16_t)~sum;
}

/* -- Frame-building helpers -------------------------------------------- */

/* Fill an Ethernet header at frame[0..13]. */
static inline void eth_fill(uint8_t *frame,
                             const uint8_t dst[6],
                             const uint8_t src[6],
                             uint16_t ethertype)
{
    eth_hdr_t *h = (eth_hdr_t *)frame;
    int i;
    for (i = 0; i < 6; i++) h->dst[i] = dst[i];
    for (i = 0; i < 6; i++) h->src[i] = src[i];
    h->ethertype = net_htons(ethertype);
}

/* Fill a 28-byte ARP packet starting at buf.  Caller provides both
 * sender and target MAC/IP; pass a zero-filled MAC for unknown targets. */
static inline void arp_fill(uint8_t *buf,
                             uint16_t        operation,
                             const uint8_t   sender_mac[6],
                             uint32_t        sender_ip,
                             const uint8_t   target_mac[6],
                             uint32_t        target_ip)
{
    arp_pkt_t *a = (arp_pkt_t *)buf;
    int i;
    a->hw_type    = net_htons(0x0001u);
    a->proto_type = net_htons(0x0800u);
    a->hw_len     = 6;
    a->proto_len  = 4;
    a->operation  = net_htons(operation);
    for (i = 0; i < 6; i++) a->sender_mac[i] = sender_mac[i];
    net_ip_write(a->sender_ip, sender_ip);
    for (i = 0; i < 6; i++) a->target_mac[i] = target_mac[i];
    net_ip_write(a->target_ip, target_ip);
}

/* Fill a 20-byte IPv4 header.  Computes and sets the header checksum. */
static inline void ipv4_fill(uint8_t *buf,
                              uint16_t total_len,
                              uint8_t  protocol,
                              uint32_t src_ip,
                              uint32_t dst_ip)
{
    ipv4_hdr_t *h = (ipv4_hdr_t *)buf;
    h->ver_ihl   = 0x45u;
    h->dscp_ecn  = 0;
    h->total_len = net_htons(total_len);
    h->ident     = 0;
    h->frag_off  = net_htons(0x4000u);  /* Don't Fragment */
    h->ttl       = 64;
    h->protocol  = protocol;
    h->checksum  = 0;
    net_ip_write(h->src_ip, src_ip);
    net_ip_write(h->dst_ip, dst_ip);
    h->checksum  = net_htons(net_checksum16(h, IPV4_HDR_SIZE));
}

/* Fill an ICMP echo request header + data_len bytes of payload.
 * If data is NULL, fills payload with 0x41 ('A').
 * Returns total bytes written (ICMP_HDR_SIZE + data_len). */
static inline int icmp_fill_echo_request(uint8_t *buf,
                                          uint16_t ident,
                                          uint16_t seq,
                                          const uint8_t *data,
                                          int data_len)
{
    icmp_hdr_t *h = (icmp_hdr_t *)buf;
    int i;
    h->type     = ICMP_ECHO_REQUEST;
    h->code     = 0;
    h->checksum = 0;
    h->ident    = net_htons(ident);
    h->seq      = net_htons(seq);
    for (i = 0; i < data_len; i++)
        buf[ICMP_HDR_SIZE + i] = data ? data[i] : (uint8_t)0x41u;
    int total = (int)ICMP_HDR_SIZE + data_len;
    h->checksum = net_htons(net_checksum16(buf, total));
    return total;
}

/* Convert an echo request (starting at ICMP header) to an echo reply
 * in-place.  Recalculates the checksum over icmp_len bytes. */
static inline void icmp_fill_echo_reply(uint8_t *icmp_buf, int icmp_len)
{
    icmp_hdr_t *h = (icmp_hdr_t *)icmp_buf;
    h->type     = ICMP_ECHO_REPLY;
    h->checksum = 0;
    h->checksum = net_htons(net_checksum16(icmp_buf, icmp_len));
}

/* Fill an 8-byte UDP header.  Sets checksum to 0 (optional per RFC 768). */
static inline void udp_fill(uint8_t *buf,
                             uint16_t src_port,
                             uint16_t dst_port,
                             uint16_t payload_len)
{
    udp_hdr_t *h = (udp_hdr_t *)buf;
    h->src_port = net_htons(src_port);
    h->dst_port = net_htons(dst_port);
    h->length   = net_htons((uint16_t)(UDP_HDR_SIZE + payload_len));
    h->checksum = 0;
}

/* Fill a 20-byte TCP header (no options).
 * data_offset_words = 5 for a plain 20-byte header.
 * Caller must compute and patch the checksum afterwards. */
static inline void tcp_fill(uint8_t *buf,
                             uint16_t src_port,
                             uint16_t dst_port,
                             uint32_t seq,
                             uint32_t ack_num,
                             uint8_t  flags,
                             uint16_t window)
{
    tcp_hdr_t *h = (tcp_hdr_t *)buf;
    h->src_port   = net_htons(src_port);
    h->dst_port   = net_htons(dst_port);
    h->seq        = net_htonl(seq);
    h->ack        = net_htonl(ack_num);
    h->data_offset= (uint8_t)(5u << 4);   /* 5 × 4 = 20 bytes, no options */
    h->flags      = flags;
    h->window     = net_htons(window);
    h->checksum   = 0;
    h->urgent     = 0;
}

/* -- ARP cache entry (used by the kernel API) --------------------------- */
typedef struct {
    uint32_t ip;
    uint8_t  mac[6];
    int      valid;
} arp_cache_entry_t;

/* -- TCP connection state ----------------------------------------------- */
typedef enum {
    TCP_STATE_CLOSED      = 0,
    TCP_STATE_LISTEN      = 1,
    TCP_STATE_SYN_SENT    = 2,   /* active open: SYN sent, awaiting SYN-ACK */
    TCP_STATE_SYN_RCVD    = 3,
    TCP_STATE_ESTABLISHED = 4,
    TCP_STATE_FIN_WAIT    = 5,
    TCP_STATE_CLOSE_WAIT  = 6,
} tcp_state_t;

/* -- Public kernel API ------------------------------------------------- */

/* net_init    -- read MAC from rtl8139 and initialise state tables.
 *               Returns 0 on success, -1 if the NIC is absent.
 *               Safe to call multiple times (idempotent). */
int         net_init(void);

/* net_set_ip  -- configure our IPv4 address and gateway (host byte order). */
void        net_set_ip(uint32_t ip, uint32_t gateway);

/* net_get_ip  -- copy current IP into *ip_out (host byte order).
 *               Returns 1 if configured, 0 if still 0.0.0.0. */
int         net_get_ip(uint32_t *ip_out);

/* net_poll    -- receive and dispatch one pending Ethernet frame.
 *               Returns 1 if a frame was processed, 0 if ring empty. */
int         net_poll(void);

/* net_send_raw -- transmit a raw Ethernet frame (thin wrapper). */
int         net_send_raw(const void *frame, uint16_t len);

/* ARP */
int         net_arp_lookup(uint32_t ip, uint8_t mac_out[6]);
void        net_arp_request(uint32_t target_ip);
void        net_arp_cache_print(void);

/* ICMP -- send echo request, poll for reply.  Returns 1 = reply, 0 = timeout. */
int         net_ping(uint32_t dst_ip);

/* UDP */
int         net_udp_send(uint32_t dst_ip,
                         uint16_t src_port, uint16_t dst_port,
                         const void *data, uint16_t len);
int         net_udp_recv(uint16_t port, void *buf, uint16_t maxlen);

/* TCP (single connection) */
int         net_tcp_listen(uint16_t port);
/* net_tcp_connect -- active open to dst_ip:dst_port (host byte order).
 * Resolves the next hop via ARP, performs the SYN / SYN-ACK / ACK handshake,
 * and blocks (polling) until ESTABLISHED or timeout.  Returns 0 on success,
 * -1 on failure (no NIC/IP, ARP failure, or handshake timeout). */
int         net_tcp_connect(uint32_t dst_ip, uint16_t dst_port);
int         net_tcp_send(const void *data, uint16_t len);
int         net_tcp_recv(void *buf, uint16_t maxlen);
/* net_tcp_readable -- bytes immediately available without blocking (0 if none). */
int         net_tcp_readable(void);
void        net_tcp_close(void);
tcp_state_t net_tcp_state(void);

/* DHCP -- discover -> offer -> request -> ack; calls net_set_ip on success.
 * Returns 0 if IP obtained, -1 on timeout. */
int         net_dhcp(void);

#endif /* _KERNEL_NET_H */
