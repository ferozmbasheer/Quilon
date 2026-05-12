/*
 * Quilon OS — Minimal TCP/IP Stack (Section 10.3)
 *
 * Implements Ethernet/ARP/IPv4/ICMP/UDP/TCP over the RTL8139 NIC.
 *
 * All state lives in module-static variables; only one UDP receive slot
 * and one TCP connection are supported simultaneously (sufficient for
 * kernel-level demos and single-client servers).
 *
 * Hardware-dependent code is guarded by #ifdef __is_kernel so that
 * test builds on the host still compile cleanly.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/net.h>

#ifdef __is_kernel
#include <kernel/rtl8139.h>
#include <kernel/waitq.h>

/* ── Constants ────────────────────────────────────────────────────────── */

#define DHCP_XID        0x51C0DE00u   /* fixed transaction ID */
#define ARP_POLL_LIMIT  2000000       /* iterations to wait for an ARP reply */
#define PING_POLL_LIMIT 5000000       /* iterations to wait for ICMP reply   */
#define DHCP_POLL_LIMIT 10000000      /* iterations to wait for DHCP reply   */

static const uint8_t g_broadcast_mac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
static const uint8_t g_zero_mac[6]      = {0,0,0,0,0,0};

/* ── Module state ─────────────────────────────────────────────────────── */

static uint8_t  g_mac[6];
static uint32_t g_ip      = 0;   /* our IPv4 address (host byte order) */
static uint32_t g_gateway = 0;   /* gateway IP */
static int      g_ready   = 0;   /* 1 after net_init() succeeds */

/* ARP cache */
static arp_cache_entry_t g_arp[NET_ARP_CACHE_SIZE];

/* UDP receive slot (one frame at a time) */
static struct {
    uint8_t  data[NET_UDP_MAX_PAYLOAD];
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t len;
    int      ready;
} g_udp_rx;

/* TCP connection (single slot) */
static struct {
    tcp_state_t state;
    uint16_t    local_port;
    uint16_t    remote_port;
    uint32_t    remote_ip;
    uint8_t     remote_mac[6];
    uint32_t    seq;        /* our next TX sequence number */
    uint32_t    ack;        /* next expected from remote   */
    uint8_t     rx_data[NET_TCP_RX_BUF];
    uint16_t    rx_len;
    int         rx_ready;
    waitq_t     rx_wq;     /* processes sleeping on net_tcp_recv      */
} g_tcp;

/* ICMP ping state */
static struct {
    uint16_t ident;
    uint16_t seq;
    int      got_reply;
} g_ping;

/* DHCP state */
static struct {
    uint32_t offered_ip;
    uint32_t server_ip;
    uint32_t acked_ip;
    int      phase;    /* 0 = waiting for OFFER, 1 = waiting for ACK */
} g_dhcp;

/* Shared frame buffers */
static uint8_t g_rx_buf[1520];
static uint8_t g_tx_buf[1520];

/* ── Private helpers ──────────────────────────────────────────────────── */

static void arp_cache_update(uint32_t ip, const uint8_t mac[6])
{
    int i;
    /* Update existing entry */
    for (i = 0; i < (int)NET_ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            int j;
            for (j = 0; j < 6; j++) g_arp[i].mac[j] = mac[j];
            return;
        }
    }
    /* Find empty slot */
    for (i = 0; i < (int)NET_ARP_CACHE_SIZE; i++) {
        if (!g_arp[i].valid) {
            g_arp[i].ip    = ip;
            g_arp[i].valid = 1;
            int j;
            for (j = 0; j < 6; j++) g_arp[i].mac[j] = mac[j];
            return;
        }
    }
    /* Evict slot 0 (simple LRU approximation) */
    g_arp[0].ip    = ip;
    g_arp[0].valid = 1;
    int j;
    for (j = 0; j < 6; j++) g_arp[0].mac[j] = mac[j];
}

/* Send an ARP reply to sender_mac / sender_ip asking for our MAC. */
static void send_arp_reply(const uint8_t *dest_mac, uint32_t dest_ip)
{
    uint8_t frame[ETH_HDR_SIZE + ARP_PKT_SIZE];
    eth_fill(frame, dest_mac, g_mac, ETHERTYPE_ARP);
    arp_fill(frame + ETH_HDR_SIZE, ARP_OP_REPLY,
             g_mac,     g_ip,
             dest_mac,  dest_ip);
    rtl8139_send(frame, (uint16_t)sizeof(frame));
}

/* Send an ICMP echo reply by flipping the type in-place and re-sending. */
static void send_icmp_reply(const uint8_t *dest_mac, uint32_t dest_ip,
                             const uint8_t *icmp_data, int icmp_len)
{
    /* Frame: Ethernet + IPv4 + ICMP */
    int total = (int)(ETH_HDR_SIZE + IPV4_HDR_SIZE) + icmp_len;
    if (total > (int)sizeof(g_tx_buf)) return;

    /* Zero the TX buffer (avoids stale bytes in payload) */
    int z;
    for (z = 0; z < total; z++) g_tx_buf[z] = 0;

    eth_fill(g_tx_buf, dest_mac, g_mac, ETHERTYPE_IPV4);
    ipv4_fill(g_tx_buf + ETH_HDR_SIZE,
              (uint16_t)(IPV4_HDR_SIZE + (uint16_t)icmp_len),
              IPPROTO_ICMP, g_ip, dest_ip);

    /* Copy ICMP header+data, flip type to REPLY, recompute checksum. */
    int i;
    for (i = 0; i < icmp_len; i++)
        g_tx_buf[ETH_HDR_SIZE + IPV4_HDR_SIZE + i] = icmp_data[i];
    icmp_fill_echo_reply(g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE, icmp_len);

    rtl8139_send(g_tx_buf, (uint16_t)total);
}

/* Send a TCP segment (with optional payload). */
static void send_tcp_segment(uint8_t flags, const void *data, uint16_t data_len)
{
    uint16_t tcp_len  = (uint16_t)(TCP_HDR_SIZE + data_len);
    uint16_t ip_total = (uint16_t)(IPV4_HDR_SIZE + tcp_len);
    uint16_t eth_tot  = (uint16_t)(ETH_HDR_SIZE + ip_total);

    if (eth_tot > (uint16_t)sizeof(g_tx_buf)) return;

    int z;
    for (z = 0; z < (int)eth_tot; z++) g_tx_buf[z] = 0;

    eth_fill(g_tx_buf, g_tcp.remote_mac, g_mac, ETHERTYPE_IPV4);
    ipv4_fill(g_tx_buf + ETH_HDR_SIZE, ip_total, IPPROTO_TCP,
              g_ip, g_tcp.remote_ip);

    uint8_t *tcp_seg = g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE;
    tcp_fill(tcp_seg,
             g_tcp.local_port, g_tcp.remote_port,
             g_tcp.seq, g_tcp.ack, flags, 4096u);

    if (data && data_len > 0) {
        const uint8_t *src = (const uint8_t *)data;
        uint8_t *dst = tcp_seg + TCP_HDR_SIZE;
        int i;
        for (i = 0; i < (int)data_len; i++) dst[i] = src[i];
    }

    /* TCP checksum (pseudo-header required). */
    ((tcp_hdr_t *)tcp_seg)->checksum =
        net_transport_checksum(g_ip, g_tcp.remote_ip,
                               IPPROTO_TCP, tcp_seg, tcp_len);

    rtl8139_send(g_tx_buf, eth_tot);
}

/* Parse DHCP option TLVs (starting after the 4-byte magic cookie). */
static void parse_dhcp_options(const uint8_t *opts, int opts_len,
                                uint8_t *msg_type,
                                uint32_t *router,
                                uint32_t *server_id)
{
    const uint8_t *p   = opts;
    const uint8_t *end = opts + opts_len;

    while (p < end) {
        uint8_t code = *p++;
        if (code == 255u) break;        /* end option */
        if (code == 0u)   continue;     /* pad option — no length byte */
        if (p >= end)     break;
        uint8_t olen = *p++;
        if (p + olen > end) break;

        switch (code) {
        case 53:
            if (olen >= 1 && msg_type) *msg_type = *p;
            break;
        case 3:
            if (olen >= 4 && router) *router = net_ip_read(p);
            break;
        case 54:
            if (olen >= 4 && server_id) *server_id = net_ip_read(p);
            break;
        default:
            break;
        }
        p += olen;
    }
}

/* Handle an inbound DHCP reply (called from handle_udp). */
static void handle_dhcp_reply(const uint8_t *data, int len)
{
    if (len < (int)sizeof(dhcp_msg_t)) return;

    const dhcp_msg_t *msg = (const dhcp_msg_t *)data;
    if (msg->op != 2u) return;                           /* must be BOOTREPLY */
    if (net_ntohl(msg->xid) != DHCP_XID) return;        /* our transaction    */

    /* Verify magic cookie */
    uint32_t magic = ((uint32_t)msg->options[0] << 24)
                   | ((uint32_t)msg->options[1] << 16)
                   | ((uint32_t)msg->options[2] <<  8)
                   |  (uint32_t)msg->options[3];
    if (magic != DHCP_MAGIC_COOKIE) return;

    uint8_t  msg_type  = 0;
    uint32_t router    = 0;
    uint32_t server_id = 0;
    parse_dhcp_options(msg->options + 4,
                       (int)sizeof(msg->options) - 4,
                       &msg_type, &router, &server_id);

    uint32_t yiaddr = net_ip_read(msg->yiaddr);
    if (router) g_gateway = router;

    if (msg_type == DHCP_MSG_OFFER && g_dhcp.phase == 0) {
        g_dhcp.offered_ip = yiaddr;
        g_dhcp.server_ip  = server_id;
    } else if (msg_type == DHCP_MSG_ACK && g_dhcp.phase == 1) {
        g_dhcp.acked_ip = yiaddr;
    }
}

/* Handle an inbound ARP frame (frame starts at Ethernet header). */
static void handle_arp(const uint8_t *frame, int len)
{
    if (len < (int)(ETH_HDR_SIZE + ARP_PKT_SIZE)) return;

    const arp_pkt_t *arp = (const arp_pkt_t *)(frame + ETH_HDR_SIZE);
    if (net_ntohs(arp->hw_type)    != 0x0001u) return;
    if (net_ntohs(arp->proto_type) != 0x0800u) return;
    if (arp->hw_len != 6u || arp->proto_len != 4u) return;

    uint32_t sender_ip = net_ip_read(arp->sender_ip);
    uint32_t target_ip = net_ip_read(arp->target_ip);
    uint16_t op        = net_ntohs(arp->operation);

    /* Always learn the sender's MAC. */
    arp_cache_update(sender_ip, arp->sender_mac);

    if (op == ARP_OP_REQUEST && g_ip != 0 && target_ip == g_ip)
        send_arp_reply(arp->sender_mac, sender_ip);
}

/* Handle an inbound ICMP segment (src_mac = bytes 6-11 of the Eth frame). */
static void handle_icmp(const uint8_t *src_mac, uint32_t src_ip,
                         const uint8_t *icmp_data, int icmp_len)
{
    if (icmp_len < (int)ICMP_HDR_SIZE) return;
    if (net_checksum16(icmp_data, icmp_len) != 0) return;  /* bad checksum */

    const icmp_hdr_t *h = (const icmp_hdr_t *)icmp_data;

    if (h->type == ICMP_ECHO_REQUEST && g_ip != 0)
        send_icmp_reply(src_mac, src_ip, icmp_data, icmp_len);

    else if (h->type == ICMP_ECHO_REPLY) {
        if (net_ntohs(h->ident) == g_ping.ident &&
            net_ntohs(h->seq)   == g_ping.seq)
            g_ping.got_reply = 1;
    }
}

/* Handle an inbound UDP segment. */
static void handle_udp(uint32_t src_ip,
                        const uint8_t *udp_data, int udp_len)
{
    if (udp_len < (int)UDP_HDR_SIZE) return;

    const udp_hdr_t *h        = (const udp_hdr_t *)udp_data;
    uint16_t         dst_port = net_ntohs(h->dst_port);
    uint16_t         src_port = net_ntohs(h->src_port);
    int              data_len = (int)net_ntohs(h->length) - (int)UDP_HDR_SIZE;
    if (data_len <= 0) return;

    /* DHCP reply (port 68) — handle regardless of whether slot is full. */
    if (dst_port == PORT_DHCP_CLIENT) {
        handle_dhcp_reply(udp_data + UDP_HDR_SIZE, data_len);
        return;
    }

    /* General UDP RX slot (one frame, first-come first-served). */
    if (!g_udp_rx.ready) {
        int copy = data_len;
        if (copy > (int)NET_UDP_MAX_PAYLOAD) copy = (int)NET_UDP_MAX_PAYLOAD;
        int i;
        for (i = 0; i < copy; i++)
            g_udp_rx.data[i] = udp_data[UDP_HDR_SIZE + i];
        g_udp_rx.src_ip   = src_ip;
        g_udp_rx.src_port = src_port;
        g_udp_rx.dst_port = dst_port;
        g_udp_rx.len      = (uint16_t)copy;
        g_udp_rx.ready    = 1;
    }
}

/* Handle an inbound TCP segment. */
static void handle_tcp(const uint8_t *src_mac, uint32_t src_ip,
                        const uint8_t *tcp_data, int tcp_len)
{
    if (tcp_len < (int)TCP_HDR_SIZE) return;

    const tcp_hdr_t *h        = (const tcp_hdr_t *)tcp_data;
    uint16_t         dst_port = net_ntohs(h->dst_port);
    uint16_t         src_port = net_ntohs(h->src_port);
    uint8_t          flags    = h->flags;
    uint32_t         seq      = net_ntohl(h->seq);
    uint32_t         ack_num  = net_ntohl(h->ack);
    int              hdr_len  = (int)(h->data_offset >> 4) * 4;
    int              data_len = tcp_len - hdr_len;

    (void)ack_num;   /* unused beyond SYN_RCVD check */

    switch (g_tcp.state) {

    case TCP_STATE_LISTEN:
        if ((flags & TCP_SYN) && !(flags & TCP_ACK) &&
            dst_port == g_tcp.local_port) {
            int i;
            for (i = 0; i < 6; i++) g_tcp.remote_mac[i] = src_mac[i];
            g_tcp.remote_ip   = src_ip;
            g_tcp.remote_port = src_port;
            g_tcp.ack         = seq + 1u;
            g_tcp.seq         = 0xC0DEB00Bu;    /* our ISN */
            send_tcp_segment(TCP_SYN | TCP_ACK, NULL, 0);
            g_tcp.seq++;   /* SYN consumes one sequence number */
            g_tcp.state = TCP_STATE_SYN_RCVD;
        }
        break;

    case TCP_STATE_SYN_RCVD:
        /* ACK of our SYN-ACK completes the handshake. */
        if ((flags & TCP_ACK) && !(flags & TCP_SYN))
            g_tcp.state = TCP_STATE_ESTABLISHED;
        break;

    case TCP_STATE_ESTABLISHED:
        if (flags & TCP_FIN) {
            g_tcp.ack += 1u;
            send_tcp_segment(TCP_FIN | TCP_ACK, NULL, 0);
            g_tcp.seq++;
            g_tcp.state = TCP_STATE_CLOSED;
        } else if ((flags & TCP_PSH) && data_len > 0) {
            if (!g_tcp.rx_ready) {
                int copy = data_len;
                if (copy > (int)NET_TCP_RX_BUF) copy = (int)NET_TCP_RX_BUF;
                int i;
                for (i = 0; i < copy; i++)
                    g_tcp.rx_data[i] = tcp_data[hdr_len + i];
                g_tcp.rx_len   = (uint16_t)copy;
                g_tcp.rx_ready = 1;
                waitq_wake_all(&g_tcp.rx_wq);   /* wake net_tcp_recv sleepers */
            }
            g_tcp.ack += (uint32_t)data_len;
            send_tcp_segment(TCP_ACK, NULL, 0);
        }
        break;

    case TCP_STATE_FIN_WAIT:
        if (flags & TCP_FIN) {
            g_tcp.ack++;
            send_tcp_segment(TCP_ACK, NULL, 0);
            g_tcp.state = TCP_STATE_CLOSED;
        }
        break;

    default:
        break;
    }
}

/* Handle an inbound IPv4 frame. */
static void handle_ipv4(const uint8_t *frame, int len)
{
    if (len < (int)(ETH_HDR_SIZE + IPV4_HDR_SIZE)) return;

    const ipv4_hdr_t *ip  = (const ipv4_hdr_t *)(frame + ETH_HDR_SIZE);
    int               ihl = (int)(ip->ver_ihl & 0x0Fu) * 4;

    if ((ip->ver_ihl >> 4) != 4u || ihl < (int)IPV4_HDR_SIZE) return;
    if (net_checksum16(ip, ihl) != 0) return;   /* bad header checksum */

    uint32_t dst_ip = net_ip_read(ip->dst_ip);
    /* Accept if broadcast or addressed to us (or no IP configured yet). */
    if (g_ip != 0 && dst_ip != g_ip && dst_ip != 0xFFFFFFFFu) return;

    uint32_t       src_ip   = net_ip_read(ip->src_ip);
    const uint8_t *src_mac  = frame + 6;   /* Ethernet source MAC */
    const uint8_t *payload  = frame + ETH_HDR_SIZE + ihl;
    int            pay_len  = (int)net_ntohs(ip->total_len) - ihl;
    if (pay_len <= 0 || pay_len > len - (int)(ETH_HDR_SIZE + ihl)) return;

    /* Update ARP cache with IPv4 sender's MAC. */
    arp_cache_update(src_ip, src_mac);

    switch (ip->protocol) {
    case IPPROTO_ICMP:
        handle_icmp(src_mac, src_ip, payload, pay_len);
        break;
    case IPPROTO_UDP:
        handle_udp(src_ip, payload, pay_len);
        break;
    case IPPROTO_TCP:
        handle_tcp(src_mac, src_ip, payload, pay_len);
        break;
    default:
        break;
    }
}

/* Build and transmit a DHCP Discover. */
static void send_dhcp_discover(void)
{
    int total = (int)(ETH_HDR_SIZE + IPV4_HDR_SIZE + UDP_HDR_SIZE + DHCP_MIN_SIZE);
    int z;
    for (z = 0; z < total; z++) g_tx_buf[z] = 0;

    eth_fill(g_tx_buf, g_broadcast_mac, g_mac, ETHERTYPE_IPV4);
    ipv4_fill(g_tx_buf + ETH_HDR_SIZE,
              (uint16_t)(IPV4_HDR_SIZE + UDP_HDR_SIZE + DHCP_MIN_SIZE),
              IPPROTO_UDP, 0u, 0xFFFFFFFFu);
    udp_fill(g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE,
             PORT_DHCP_CLIENT, PORT_DHCP_SERVER,
             (uint16_t)DHCP_MIN_SIZE);

    dhcp_msg_t *msg =
        (dhcp_msg_t *)(g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE + UDP_HDR_SIZE);
    msg->op    = 1u;
    msg->htype = 1u;
    msg->hlen  = 6u;
    msg->xid   = net_htonl(DHCP_XID);
    msg->flags = net_htons(0x8000u);  /* broadcast */
    int i;
    for (i = 0; i < 6; i++) msg->chaddr[i] = g_mac[i];

    /* Options: magic cookie + message type DISCOVER + parameter request */
    msg->options[0] = 0x63u; msg->options[1] = 0x82u;
    msg->options[2] = 0x53u; msg->options[3] = 0x63u;
    msg->options[4] = 53u; msg->options[5] = 1u; msg->options[6] = DHCP_MSG_DISCOVER;
    msg->options[7] = 55u; msg->options[8] = 4u;
    msg->options[9]  = 1u;   /* subnet mask */
    msg->options[10] = 3u;   /* router      */
    msg->options[11] = 6u;   /* DNS server  */
    msg->options[12] = 51u;  /* lease time  */
    msg->options[13] = 255u; /* end         */

    rtl8139_send(g_tx_buf, (uint16_t)total);
}

/* Build and transmit a DHCP Request for offered_ip from server_ip. */
static void send_dhcp_request(uint32_t offered_ip, uint32_t server_ip)
{
    int total = (int)(ETH_HDR_SIZE + IPV4_HDR_SIZE + UDP_HDR_SIZE + DHCP_MIN_SIZE);
    int z;
    for (z = 0; z < total; z++) g_tx_buf[z] = 0;

    eth_fill(g_tx_buf, g_broadcast_mac, g_mac, ETHERTYPE_IPV4);
    ipv4_fill(g_tx_buf + ETH_HDR_SIZE,
              (uint16_t)(IPV4_HDR_SIZE + UDP_HDR_SIZE + DHCP_MIN_SIZE),
              IPPROTO_UDP, 0u, 0xFFFFFFFFu);
    udp_fill(g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE,
             PORT_DHCP_CLIENT, PORT_DHCP_SERVER,
             (uint16_t)DHCP_MIN_SIZE);

    dhcp_msg_t *msg =
        (dhcp_msg_t *)(g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE + UDP_HDR_SIZE);
    msg->op    = 1u;
    msg->htype = 1u;
    msg->hlen  = 6u;
    msg->xid   = net_htonl(DHCP_XID);
    msg->flags = net_htons(0x8000u);
    int i;
    for (i = 0; i < 6; i++) msg->chaddr[i] = g_mac[i];

    msg->options[0] = 0x63u; msg->options[1] = 0x82u;
    msg->options[2] = 0x53u; msg->options[3] = 0x63u;
    msg->options[4] = 53u; msg->options[5] = 1u; msg->options[6] = DHCP_MSG_REQUEST;
    msg->options[7] = 50u; msg->options[8] = 4u;
    net_ip_write(msg->options + 9, offered_ip);
    msg->options[13] = 54u; msg->options[14] = 4u;
    net_ip_write(msg->options + 15, server_ip);
    msg->options[19] = 255u;

    rtl8139_send(g_tx_buf, (uint16_t)total);
}

/* ── Public API ───────────────────────────────────────────────────────── */

int net_init(void)
{
    if (g_ready) return 0;   /* already initialised */

    if (!rtl8139_is_ready()) {
        if (rtl8139_init() != 0) return -1;
    }
    rtl8139_get_mac(g_mac);

    int i;
    for (i = 0; i < (int)NET_ARP_CACHE_SIZE; i++) g_arp[i].valid = 0;

    g_udp_rx.ready  = 0;
    g_tcp.state     = TCP_STATE_CLOSED;
    g_tcp.rx_ready  = 0;
    g_ping.got_reply= 0;
    g_dhcp.offered_ip = 0;
    g_dhcp.acked_ip   = 0;
    g_ready = 1;
    return 0;
}

void net_set_ip(uint32_t ip, uint32_t gateway)
{
    g_ip      = ip;
    g_gateway = gateway;
}

int net_get_ip(uint32_t *ip_out)
{
    if (ip_out) *ip_out = g_ip;
    return (g_ip != 0) ? 1 : 0;
}

int net_poll(void)
{
    if (!g_ready) return 0;

    int n = rtl8139_recv(g_rx_buf, (uint16_t)sizeof(g_rx_buf));
    if (n <= 0) return 0;
    if (n < (int)ETH_HDR_SIZE) return 0;

    const eth_hdr_t *eth  = (const eth_hdr_t *)g_rx_buf;
    uint16_t         type = net_ntohs(eth->ethertype);

    if (type == ETHERTYPE_ARP)
        handle_arp(g_rx_buf, n);
    else if (type == ETHERTYPE_IPV4)
        handle_ipv4(g_rx_buf, n);

    return 1;
}

int net_send_raw(const void *frame, uint16_t len)
{
    return rtl8139_send(frame, len);
}

int net_arp_lookup(uint32_t ip, uint8_t mac_out[6])
{
    int i;
    /* Check cache first. */
    for (i = 0; i < (int)NET_ARP_CACHE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            int j;
            for (j = 0; j < 6; j++) mac_out[j] = g_arp[i].mac[j];
            return 0;
        }
    }
    /* Not cached — send ARP request and poll for reply. */
    net_arp_request(ip);
    for (int iter = 0; iter < ARP_POLL_LIMIT; iter++) {
        net_poll();
        for (i = 0; i < (int)NET_ARP_CACHE_SIZE; i++) {
            if (g_arp[i].valid && g_arp[i].ip == ip) {
                int j;
                for (j = 0; j < 6; j++) mac_out[j] = g_arp[i].mac[j];
                return 0;
            }
        }
    }
    return -1;
}

void net_arp_request(uint32_t target_ip)
{
    if (!g_ready) return;
    uint8_t frame[ETH_HDR_SIZE + ARP_PKT_SIZE];
    eth_fill(frame, g_broadcast_mac, g_mac, ETHERTYPE_ARP);
    arp_fill(frame + ETH_HDR_SIZE, ARP_OP_REQUEST,
             g_mac,        g_ip,
             g_zero_mac,   target_ip);
    rtl8139_send(frame, (uint16_t)sizeof(frame));
}

void net_arp_cache_print(void)
{
    int i;
    printf("ARP cache (%d slots):\r\n", (int)NET_ARP_CACHE_SIZE);
    for (i = 0; i < (int)NET_ARP_CACHE_SIZE; i++) {
        if (!g_arp[i].valid) {
            printf("  [%d] empty\r\n", i);
            continue;
        }
        uint32_t ip = g_arp[i].ip;
        const uint8_t *m = g_arp[i].mac;
        printf("  [%d] %d.%d.%d.%d  →  %02x:%02x:%02x:%02x:%02x:%02x\r\n",
               i,
               (int)((ip >> 24) & 0xFF), (int)((ip >> 16) & 0xFF),
               (int)((ip >>  8) & 0xFF), (int)(ip & 0xFF),
               (unsigned)m[0], (unsigned)m[1], (unsigned)m[2],
               (unsigned)m[3], (unsigned)m[4], (unsigned)m[5]);
    }
}

int net_ping(uint32_t dst_ip)
{
    if (!g_ready || g_ip == 0) return 0;

    uint8_t dst_mac[6];
    /* Route through gateway if dst is not on the local subnet. */
    uint32_t next_hop = dst_ip;
    /* Simple /24 check: same class-C subnet as us → direct, else via gateway. */
    if ((dst_ip & 0xFFFFFF00u) != (g_ip & 0xFFFFFF00u) && g_gateway != 0)
        next_hop = g_gateway;

    if (net_arp_lookup(next_hop, dst_mac) != 0) return 0;

    g_ping.ident     = 0x5151u;
    g_ping.seq       = (uint16_t)(g_ping.seq + 1u);
    g_ping.got_reply = 0;

    /* Build Ethernet + IPv4 + ICMP echo request */
    uint8_t echo_data[NET_ICMP_ECHO_DATA];
    int i;
    for (i = 0; i < (int)NET_ICMP_ECHO_DATA; i++) echo_data[i] = (uint8_t)i;

    int icmp_len = icmp_fill_echo_request(
        g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE,
        g_ping.ident, g_ping.seq,
        echo_data, (int)NET_ICMP_ECHO_DATA);

    eth_fill(g_tx_buf, dst_mac, g_mac, ETHERTYPE_IPV4);
    ipv4_fill(g_tx_buf + ETH_HDR_SIZE,
              (uint16_t)(IPV4_HDR_SIZE + (uint16_t)icmp_len),
              IPPROTO_ICMP, g_ip, dst_ip);

    rtl8139_send(g_tx_buf,
                 (uint16_t)(ETH_HDR_SIZE + IPV4_HDR_SIZE + (uint16_t)icmp_len));

    for (int iter = 0; iter < PING_POLL_LIMIT; iter++) {
        net_poll();
        if (g_ping.got_reply) return 1;
    }
    return 0;
}

int net_udp_send(uint32_t dst_ip,
                  uint16_t src_port, uint16_t dst_port,
                  const void *data, uint16_t len)
{
    if (!g_ready || g_ip == 0) return -1;
    if (len > NET_UDP_MAX_PAYLOAD) return -1;

    uint8_t dst_mac[6];
    uint32_t next_hop = dst_ip;
    if ((dst_ip & 0xFFFFFF00u) != (g_ip & 0xFFFFFF00u) && g_gateway != 0)
        next_hop = g_gateway;
    /* Broadcast: skip ARP. */
    if (dst_ip == 0xFFFFFFFFu) {
        int j;
        for (j = 0; j < 6; j++) dst_mac[j] = 0xFFu;
    } else if (net_arp_lookup(next_hop, dst_mac) != 0) {
        return -1;
    }

    uint16_t ip_total  = (uint16_t)(IPV4_HDR_SIZE + UDP_HDR_SIZE + len);
    uint16_t eth_total = (uint16_t)(ETH_HDR_SIZE + ip_total);
    if (eth_total > (uint16_t)sizeof(g_tx_buf)) return -1;

    int z;
    for (z = 0; z < (int)eth_total; z++) g_tx_buf[z] = 0;

    eth_fill(g_tx_buf, dst_mac, g_mac, ETHERTYPE_IPV4);
    ipv4_fill(g_tx_buf + ETH_HDR_SIZE, ip_total, IPPROTO_UDP, g_ip, dst_ip);
    udp_fill(g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE, src_port, dst_port, len);

    const uint8_t *src = (const uint8_t *)data;
    uint8_t *dst_buf = g_tx_buf + ETH_HDR_SIZE + IPV4_HDR_SIZE + UDP_HDR_SIZE;
    int i;
    for (i = 0; i < (int)len; i++) dst_buf[i] = src[i];

    rtl8139_send(g_tx_buf, eth_total);
    return 0;
}

int net_udp_recv(uint16_t port, void *buf, uint16_t maxlen)
{
    /* Drain receive ring for a short window. */
    int poll;
    for (poll = 0; poll < 100; poll++) net_poll();

    if (!g_udp_rx.ready) return 0;
    if (g_udp_rx.dst_port != port) return 0;

    uint16_t copy = g_udp_rx.len;
    if (copy > maxlen) copy = maxlen;
    uint8_t *dst = (uint8_t *)buf;
    int i;
    for (i = 0; i < (int)copy; i++) dst[i] = g_udp_rx.data[i];
    g_udp_rx.ready = 0;
    return (int)copy;
}

int net_tcp_listen(uint16_t port)
{
    if (!g_ready || g_ip == 0) return -1;
    g_tcp.state       = TCP_STATE_LISTEN;
    g_tcp.local_port  = port;
    g_tcp.rx_ready    = 0;
    g_tcp.rx_len      = 0;
    g_tcp.rx_wq       = (waitq_t)WAITQ_INIT;
    return 0;
}

int net_tcp_send(const void *data, uint16_t len)
{
    if (g_tcp.state != TCP_STATE_ESTABLISHED) return -1;
    if (len > (uint16_t)(sizeof(g_tx_buf) - ETH_HDR_SIZE - IPV4_HDR_SIZE - TCP_HDR_SIZE))
        return -1;
    send_tcp_segment(TCP_PSH | TCP_ACK, data, len);
    g_tcp.seq += (uint32_t)len;
    return 0;
}

int net_tcp_recv(void *buf, uint16_t maxlen)
{
    /* Drain the NIC ring and sleep between polls instead of spinning.
     * handle_tcp calls waitq_wake_all(&g_tcp.rx_wq) when data arrives,
     * so if IRQ-driven networking is ever added we wake immediately.   */
#ifdef __is_kernel
    for (int iter = 0; iter < 100 && !g_tcp.rx_ready; iter++) {
        net_poll();
        if (!g_tcp.rx_ready)
            waitq_sleep(&g_tcp.rx_wq);
    }
#else
    for (int poll = 0; poll < 100; poll++) net_poll();
#endif

    if (!g_tcp.rx_ready) return 0;
    uint16_t copy = g_tcp.rx_len;
    if (copy > maxlen) copy = maxlen;
    uint8_t *dst = (uint8_t *)buf;
    int i;
    for (i = 0; i < (int)copy; i++) dst[i] = g_tcp.rx_data[i];
    g_tcp.rx_ready = 0;
    return (int)copy;
}

void net_tcp_close(void)
{
    if (g_tcp.state == TCP_STATE_ESTABLISHED) {
        send_tcp_segment(TCP_FIN | TCP_ACK, NULL, 0);
        g_tcp.seq++;
        g_tcp.state = TCP_STATE_FIN_WAIT;
        /* Poll briefly for the ACK. */
        int iter;
        for (iter = 0; iter < 1000000; iter++) {
            net_poll();
            if (g_tcp.state == TCP_STATE_CLOSED) break;
        }
    }
    g_tcp.state = TCP_STATE_CLOSED;
}

tcp_state_t net_tcp_state(void)
{
    return g_tcp.state;
}

int net_dhcp(void)
{
    if (!g_ready) return -1;

    g_dhcp.offered_ip = 0;
    g_dhcp.server_ip  = 0;
    g_dhcp.acked_ip   = 0;
    g_dhcp.phase      = 0;

    /* Phase 0: Discover → Offer */
    send_dhcp_discover();
    int i;
    for (i = 0; i < DHCP_POLL_LIMIT; i++) {
        net_poll();
        if (g_dhcp.offered_ip != 0) break;
    }
    if (g_dhcp.offered_ip == 0) return -1;

    /* Phase 1: Request → ACK */
    g_dhcp.phase = 1;
    send_dhcp_request(g_dhcp.offered_ip, g_dhcp.server_ip);
    for (i = 0; i < DHCP_POLL_LIMIT; i++) {
        net_poll();
        if (g_dhcp.acked_ip != 0) break;
    }
    if (g_dhcp.acked_ip == 0) return -1;

    net_set_ip(g_dhcp.acked_ip, g_gateway);
    return 0;
}

#endif /* __is_kernel */
