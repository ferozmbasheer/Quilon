/*
 * Quilon OS -- unit tests for the TCP/IP stack helpers (section 10.3)
 *
 * Tests all pure-C static-inline functions in kernel/include/kernel/net.h.
 * No kernel code or x86 I/O is used -- everything compiles on the host.
 *
 * Build: cc -Wall -Wextra -g -std=c11 -I../kernel/include -o bin/test_net test_net.c
 */

#include "framework.h"
#include <kernel/net.h>
#include <string.h>

/* -- Byte-order helpers ------------------------------------------------- */

static void test_htons(void)
{
    ASSERT_EQ(net_htons(0x0800u), 0x0008u, "htons 0x0800");
    ASSERT_EQ(net_htons(0x0806u), 0x0608u, "htons 0x0806 (ARP)");
    ASSERT_EQ(net_htons(0x0000u), 0x0000u, "htons 0");
    ASSERT_EQ(net_htons(0xFFFFu), 0xFFFFu, "htons 0xFFFF");
    ASSERT_EQ(net_htons(0x1234u), 0x3412u, "htons 0x1234");
    ASSERT_EQ(net_ntohs(net_htons(0xABCDu)), 0xABCDu, "ntohs(htons) roundtrip");
}

static void test_htonl(void)
{
    ASSERT_EQ(net_htonl(0x0A00020Fu), 0x0F02000Au, "htonl 10.0.2.15");
    ASSERT_EQ(net_htonl(0x00000000u), 0x00000000u, "htonl 0");
    ASSERT_EQ(net_htonl(0xFFFFFFFFu), 0xFFFFFFFFu, "htonl broadcast");
    ASSERT_EQ(net_htonl(0x12345678u), 0x78563412u, "htonl 0x12345678");
    ASSERT_EQ(net_ntohl(net_htonl(0xDEADBEEFu)), 0xDEADBEEFu,
              "ntohl(htonl) roundtrip");
}

/* -- IP address helpers ------------------------------------------------- */

static void test_ip_read_write(void)
{
    uint8_t buf[4] = {10, 0, 2, 15};
    uint32_t ip = net_ip_read(buf);
    ASSERT_EQ(ip, 0x0A00020Fu, "ip_read 10.0.2.15");

    uint8_t out[4] = {0};
    net_ip_write(out, 0xC0A80101u);   /* 192.168.1.1 */
    ASSERT_EQ(out[0], 192u, "ip_write byte0");
    ASSERT_EQ(out[1], 168u, "ip_write byte1");
    ASSERT_EQ(out[2],   1u, "ip_write byte2");
    ASSERT_EQ(out[3],   1u, "ip_write byte3");

    /* roundtrip */
    uint8_t rt[4];
    net_ip_write(rt, ip);
    ASSERT_EQ(net_ip_read(rt), ip, "ip roundtrip");
}

/* -- RFC 1071 checksum -------------------------------------------------- */

static void test_checksum16(void)
{
    /* All-zero 20-byte buffer: sum=0, ~0 = 0xFFFF. */
    uint8_t zero[20];
    memset(zero, 0, sizeof(zero));
    ASSERT_EQ(net_checksum16(zero, 20), 0xFFFFu, "checksum of zeros");

    /* Known IPv4 header checksum.
     * Header (no options): version=4, IHL=5, TTL=64, proto=UDP(17),
     * src=192.168.1.1, dst=192.168.1.2.  Checksum field = 0.
     * We compute, store, then verify. */
    uint8_t hdr[20] = {
        0x45, 0x00,              /* ver_ihl, dscp_ecn */
        0x00, 0x28,              /* total_len = 40 */
        0x00, 0x00,              /* ident */
        0x40, 0x00,              /* flags=DF, frag_off=0 */
        0x40, 0x11,              /* ttl=64, proto=UDP */
        0x00, 0x00,              /* checksum placeholder */
        0xC0, 0xA8, 0x01, 0x01, /* src 192.168.1.1 */
        0xC0, 0xA8, 0x01, 0x02, /* dst 192.168.1.2 */
    };
    uint16_t csum = net_checksum16(hdr, 20);
    ASSERT_NE(csum, 0u, "checksum of IPv4 hdr is non-zero before store");

    /* Store it and re-verify: result must be 0x0000. */
    hdr[10] = (uint8_t)(csum >> 8);
    hdr[11] = (uint8_t)(csum & 0xFF);
    ASSERT_EQ(net_checksum16(hdr, 20), 0x0000u,
              "verification of stored checksum returns 0");

    /* Odd length: 3-byte buffer {0x01, 0x02, 0x03} -> odd byte zero-padded. */
    uint8_t odd[3] = {0x01, 0x02, 0x03};
    uint16_t odd_csum = net_checksum16(odd, 3);
    ASSERT_NE(odd_csum, 0u, "odd-length checksum non-zero");
    /* Verify: manually pad and re-check. */
    uint8_t odd4[4] = {0x01, 0x02, 0x03, 0x00};
    uint16_t odd4_csum = net_checksum16(odd4, 3);   /* same code path */
    ASSERT_EQ(odd_csum, odd4_csum, "odd-length matches padded");
}

/* -- TCP/UDP pseudo-header checksum ------------------------------------ */

static void test_transport_checksum(void)
{
    /* Build a minimal UDP segment (8-byte header, no payload).
     * Pseudo-header: src=10.0.2.15, dst=10.0.2.2, proto=17, len=8. */
    uint8_t udp[8] = {
        0x00, 0x44,   /* src port 68 (DHCP client) */
        0x00, 0x43,   /* dst port 67 (DHCP server) */
        0x00, 0x08,   /* length = 8 */
        0x00, 0x00,   /* checksum = 0 */
    };
    uint32_t src = 0x0A00020Fu;   /* 10.0.2.15 */
    uint32_t dst = 0x0A000202u;   /* 10.0.2.2  */
    uint16_t csum = net_transport_checksum(src, dst, IPPROTO_UDP, udp, 8);
    ASSERT_NE(csum, 0u, "UDP pseudo-header checksum non-zero");

    /* Store and verify. */
    udp[6] = (uint8_t)(csum >> 8);
    udp[7] = (uint8_t)(csum & 0xFF);
    uint16_t verify = net_transport_checksum(src, dst, IPPROTO_UDP, udp, 8);
    ASSERT_EQ(verify, 0x0000u, "UDP pseudo-header verify returns 0");

    /* Same test with TCP protocol number. */
    uint8_t tcp[20];
    memset(tcp, 0, sizeof(tcp));
    /* src=1234, dst=80, seq=1, ack=0, data_offset=5<<4, flags=SYN */
    tcp[0] = 0x04; tcp[1] = 0xD2;   /* src port 1234 */
    tcp[2] = 0x00; tcp[3] = 0x50;   /* dst port 80   */
    tcp[4] = 0x00; tcp[5] = 0x00; tcp[6] = 0x00; tcp[7] = 0x01; /* seq=1 */
    tcp[12] = (5u << 4);             /* data_offset   */
    tcp[13] = TCP_SYN;               /* flags         */
    tcp[14] = 0x20; tcp[15] = 0x00; /* window=8192   */

    uint16_t tcsum = net_transport_checksum(src, dst, IPPROTO_TCP, tcp, 20);
    ASSERT_NE(tcsum, 0u, "TCP pseudo-header checksum non-zero");

    tcp[16] = (uint8_t)(tcsum >> 8);
    tcp[17] = (uint8_t)(tcsum & 0xFF);
    ASSERT_EQ(net_transport_checksum(src, dst, IPPROTO_TCP, tcp, 20),
              0x0000u, "TCP pseudo-header verify returns 0");
}

/* -- Ethernet header --------------------------------------------------- */

static void test_eth_fill(void)
{
    uint8_t frame[14];
    memset(frame, 0xAA, sizeof(frame));

    uint8_t dst[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t src[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    eth_fill(frame, dst, src, ETHERTYPE_ARP);

    /* Destination MAC */
    int i;
    for (i = 0; i < 6; i++)
        ASSERT_EQ(frame[i], 0xFFu, "eth dst byte");
    /* Source MAC */
    ASSERT_EQ(frame[6],  0x52u, "eth src[0]");
    ASSERT_EQ(frame[7],  0x54u, "eth src[1]");
    ASSERT_EQ(frame[11], 0x56u, "eth src[5]");
    /* EtherType ARP = 0x0806 in big-endian */
    ASSERT_EQ(frame[12], 0x08u, "eth ethertype hi");
    ASSERT_EQ(frame[13], 0x06u, "eth ethertype lo");

    /* IPv4 ethertype */
    uint8_t f2[14];
    eth_fill(f2, dst, src, ETHERTYPE_IPV4);
    ASSERT_EQ(f2[12], 0x08u, "ipv4 ethertype hi");
    ASSERT_EQ(f2[13], 0x00u, "ipv4 ethertype lo");
}

/* -- ARP packet -------------------------------------------------------- */

static void test_arp_fill(void)
{
    uint8_t buf[28];
    memset(buf, 0, sizeof(buf));

    uint8_t smac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint8_t tmac[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    uint32_t sender_ip = 0x0A00020Fu;   /* 10.0.2.15 */
    uint32_t target_ip = 0x0A000202u;   /* 10.0.2.2  */

    arp_fill(buf, ARP_OP_REQUEST, smac, sender_ip, tmac, target_ip);

    arp_pkt_t *a = (arp_pkt_t *)buf;

    /* hw_type = 1 (Ethernet) in network byte order */
    ASSERT_EQ(a->hw_type,   net_htons(1u),          "arp hw_type");
    ASSERT_EQ(a->proto_type, net_htons(0x0800u),     "arp proto_type");
    ASSERT_EQ(a->hw_len,     6u,                      "arp hw_len");
    ASSERT_EQ(a->proto_len,  4u,                      "arp proto_len");
    ASSERT_EQ(a->operation,  net_htons(ARP_OP_REQUEST), "arp op request");

    /* sender MAC and IP */
    ASSERT_EQ(a->sender_mac[0], smac[0], "arp sender_mac[0]");
    ASSERT_EQ(a->sender_mac[5], smac[5], "arp sender_mac[5]");
    ASSERT_EQ(net_ip_read(a->sender_ip), sender_ip, "arp sender_ip");

    /* target IP */
    ASSERT_EQ(net_ip_read(a->target_ip), target_ip, "arp target_ip");

    /* ARP reply */
    arp_fill(buf, ARP_OP_REPLY, smac, sender_ip, smac, target_ip);
    ASSERT_EQ(a->operation, net_htons(ARP_OP_REPLY), "arp op reply");
}

/* -- IPv4 header ------------------------------------------------------- */

static void test_ipv4_fill(void)
{
    uint8_t buf[20];
    memset(buf, 0, sizeof(buf));

    uint32_t src = 0x0A00020Fu;   /* 10.0.2.15 */
    uint32_t dst = 0x0A000202u;   /* 10.0.2.2  */
    ipv4_fill(buf, 40u, IPPROTO_UDP, src, dst);

    ipv4_hdr_t *h = (ipv4_hdr_t *)buf;

    ASSERT_EQ(h->ver_ihl,  0x45u,              "ipv4 ver_ihl");
    ASSERT_EQ(h->protocol, IPPROTO_UDP,         "ipv4 proto UDP");
    ASSERT_EQ(net_ntohs(h->total_len), 40u,     "ipv4 total_len");
    ASSERT_EQ(h->ttl, 64u,                      "ipv4 ttl");
    ASSERT_EQ(net_ip_read(h->src_ip), src,      "ipv4 src");
    ASSERT_EQ(net_ip_read(h->dst_ip), dst,      "ipv4 dst");

    /* Checksum must be valid: verifying over the header should yield 0. */
    ASSERT_EQ(net_checksum16(buf, IPV4_HDR_SIZE), 0x0000u,
              "ipv4 checksum valid after fill");

    /* ICMP header */
    ipv4_fill(buf, 28u, IPPROTO_ICMP, src, dst);
    ASSERT_EQ(h->protocol, IPPROTO_ICMP, "ipv4 proto ICMP");
    ASSERT_EQ(net_checksum16(buf, IPV4_HDR_SIZE), 0x0000u,
              "ipv4 checksum valid for ICMP");
}

/* -- ICMP echo request / reply ----------------------------------------- */

static void test_icmp_fill(void)
{
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));

    int total = icmp_fill_echo_request(buf, 0x1234u, 1u, NULL, 8);

    ASSERT_EQ(total, (int)(ICMP_HDR_SIZE + 8), "icmp total size");

    icmp_hdr_t *h = (icmp_hdr_t *)buf;
    ASSERT_EQ(h->type, ICMP_ECHO_REQUEST, "icmp type request");
    ASSERT_EQ(h->code, 0u,                "icmp code 0");
    ASSERT_EQ(net_ntohs(h->ident), 0x1234u, "icmp ident");
    ASSERT_EQ(net_ntohs(h->seq),   1u,      "icmp seq");

    /* Default payload should be 0x41 ('A'). */
    ASSERT_EQ(buf[ICMP_HDR_SIZE], 0x41u, "icmp default payload byte");

    /* Checksum must be valid. */
    ASSERT_EQ(net_checksum16(buf, total), 0x0000u,
              "icmp request checksum valid");

    /* Convert to reply in-place. */
    icmp_fill_echo_reply(buf, total);
    ASSERT_EQ(h->type, ICMP_ECHO_REPLY, "icmp type reply after fill");
    ASSERT_EQ(net_checksum16(buf, total), 0x0000u,
              "icmp reply checksum valid");

    /* Custom payload. */
    uint8_t data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    total = icmp_fill_echo_request(buf, 0x0001u, 2u, data, 4);
    ASSERT_EQ(buf[ICMP_HDR_SIZE + 0], 0xDEu, "icmp custom payload[0]");
    ASSERT_EQ(buf[ICMP_HDR_SIZE + 3], 0xEFu, "icmp custom payload[3]");
    ASSERT_EQ(net_checksum16(buf, total), 0x0000u,
              "icmp custom payload checksum valid");
}

/* -- UDP header -------------------------------------------------------- */

static void test_udp_fill(void)
{
    uint8_t buf[8];
    memset(buf, 0xFF, sizeof(buf));

    udp_fill(buf, 68u, 67u, 10u);   /* src=68, dst=67, payload=10 */

    udp_hdr_t *h = (udp_hdr_t *)buf;
    ASSERT_EQ(net_ntohs(h->src_port), 68u,                   "udp src port");
    ASSERT_EQ(net_ntohs(h->dst_port), 67u,                   "udp dst port");
    ASSERT_EQ(net_ntohs(h->length),   (uint16_t)(8u + 10u),  "udp length");
    ASSERT_EQ(h->checksum, 0u,                                "udp checksum 0");
}

/* -- TCP header -------------------------------------------------------- */

static void test_tcp_fill(void)
{
    uint8_t buf[20];
    memset(buf, 0xFF, sizeof(buf));

    tcp_fill(buf, 12345u, 80u, 0x00C0DEB0u, 0u, TCP_SYN, 8192u);

    tcp_hdr_t *h = (tcp_hdr_t *)buf;
    ASSERT_EQ(net_ntohs(h->src_port), 12345u,      "tcp src port");
    ASSERT_EQ(net_ntohs(h->dst_port), 80u,          "tcp dst port");
    ASSERT_EQ(net_ntohl(h->seq),      0x00C0DEB0u,  "tcp seq");
    ASSERT_EQ(net_ntohl(h->ack),      0u,            "tcp ack");
    ASSERT_EQ(h->data_offset,         (uint8_t)(5u << 4), "tcp data_offset");
    ASSERT_EQ(h->flags,               TCP_SYN,       "tcp SYN flag");
    ASSERT_EQ(net_ntohs(h->window),   8192u,         "tcp window");
    ASSERT_EQ(h->checksum,            0u,             "tcp checksum 0 before compute");
    ASSERT_EQ(h->urgent,              0u,             "tcp urgent 0");

    /* ACK+PSH combination */
    tcp_fill(buf, 80u, 12345u, 1u, 0xDEADBEEFu, TCP_ACK | TCP_PSH, 4096u);
    ASSERT_EQ(h->flags, (uint8_t)(TCP_ACK | TCP_PSH), "tcp ACK|PSH flags");
    ASSERT_EQ(net_ntohl(h->ack), 0xDEADBEEFu, "tcp ack field");
}

/* -- Constants sanity checks ------------------------------------------- */

static void test_constants(void)
{
    ASSERT_EQ(ETH_HDR_SIZE,   14u, "ETH_HDR_SIZE");
    ASSERT_EQ(IPV4_HDR_SIZE,  20u, "IPV4_HDR_SIZE");
    ASSERT_EQ(ICMP_HDR_SIZE,   8u, "ICMP_HDR_SIZE");
    ASSERT_EQ(UDP_HDR_SIZE,    8u, "UDP_HDR_SIZE");
    ASSERT_EQ(TCP_HDR_SIZE,   20u, "TCP_HDR_SIZE");
    ASSERT_EQ(DHCP_MIN_SIZE, 300u, "DHCP_MIN_SIZE");
    ASSERT_EQ(ETH_MAC_LEN,     6u, "ETH_MAC_LEN");

    ASSERT_EQ(ETHERTYPE_IPV4, 0x0800u, "ETHERTYPE_IPV4");
    ASSERT_EQ(ETHERTYPE_ARP,  0x0806u, "ETHERTYPE_ARP");
    ASSERT_EQ(IPPROTO_ICMP,   1u,      "IPPROTO_ICMP");
    ASSERT_EQ(IPPROTO_TCP,    6u,      "IPPROTO_TCP");
    ASSERT_EQ(IPPROTO_UDP,   17u,      "IPPROTO_UDP");

    /* TCP flag bits must not overlap. */
    ASSERT_EQ((TCP_FIN & TCP_SYN), 0u, "FIN/SYN no overlap");
    ASSERT_EQ((TCP_SYN & TCP_ACK), 0u, "SYN/ACK no overlap");
    ASSERT_EQ((TCP_RST & TCP_PSH), 0u, "RST/PSH no overlap");

    /* DHCP magic cookie */
    ASSERT_EQ(DHCP_MAGIC_COOKIE, 0x63825363u, "DHCP magic cookie");

    /* dhcp_msg_t must be exactly DHCP_MIN_SIZE bytes. */
    ASSERT_EQ(sizeof(dhcp_msg_t), (size_t)DHCP_MIN_SIZE,
              "dhcp_msg_t size == DHCP_MIN_SIZE");
}

/* -- Full frame assembly: Ethernet + IPv4 + ICMP ----------------------- */

static void test_frame_assembly(void)
{
    uint8_t frame[ETH_HDR_SIZE + IPV4_HDR_SIZE + ICMP_HDR_SIZE + 8];
    memset(frame, 0, sizeof(frame));

    uint8_t smac[6] = {0x52, 0x54, 0x00, 0xAA, 0xBB, 0xCC};
    uint8_t dmac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint32_t src_ip = 0x0A00020Fu;
    uint32_t dst_ip = 0x0A000202u;

    /* Layer 2 */
    eth_fill(frame, dmac, smac, ETHERTYPE_IPV4);

    /* Layer 4 ICMP (compute before IPv4 so we know total_len) */
    uint8_t *icmp = frame + ETH_HDR_SIZE + IPV4_HDR_SIZE;
    int icmp_len = icmp_fill_echo_request(icmp, 1u, 1u, NULL, 8);

    /* Layer 3 */
    uint8_t *ip = frame + ETH_HDR_SIZE;
    ipv4_fill(ip, (uint16_t)(IPV4_HDR_SIZE + icmp_len), IPPROTO_ICMP,
              src_ip, dst_ip);

    /* Ethernet ethertype check */
    ASSERT_EQ(frame[12], 0x08u, "frame eth ethertype hi");
    ASSERT_EQ(frame[13], 0x00u, "frame eth ethertype lo");

    /* IPv4 checksum still valid */
    ASSERT_EQ(net_checksum16(ip, IPV4_HDR_SIZE), 0x0000u,
              "frame ipv4 checksum valid");

    /* ICMP checksum still valid */
    ASSERT_EQ(net_checksum16(icmp, icmp_len), 0x0000u,
              "frame icmp checksum valid");

    /* Protocol field */
    ipv4_hdr_t *ih = (ipv4_hdr_t *)ip;
    ASSERT_EQ(ih->protocol, IPPROTO_ICMP, "frame proto ICMP");
}

/* -- Entry point ------------------------------------------------------- */

int main(void)
{
    RUN_SUITE(test_constants);
    RUN_SUITE(test_htons);
    RUN_SUITE(test_htonl);
    RUN_SUITE(test_ip_read_write);
    RUN_SUITE(test_checksum16);
    RUN_SUITE(test_transport_checksum);
    RUN_SUITE(test_eth_fill);
    RUN_SUITE(test_arp_fill);
    RUN_SUITE(test_ipv4_fill);
    RUN_SUITE(test_icmp_fill);
    RUN_SUITE(test_udp_fill);
    RUN_SUITE(test_tcp_fill);
    RUN_SUITE(test_frame_assembly);
    TEST_SUMMARY();
}
