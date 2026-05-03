/*
 * Quilon OS — RTL8139 Unit Tests (Section 10.2)
 *
 * Tests the pure-C helpers in rtl8139.h that can run on the host without
 * x86 I/O instructions:
 *
 *   1. Constants      — register offsets, buffer sizes, flag bits
 *   2. rtl8139_bar0_iobase()    — I/O base extraction from a raw BAR0 value
 *   3. rtl8139_rx_advance()     — RX ring position advancement + wrap
 *   4. rtl8139_capr_value()     — CAPR register encoding (pos - 16 quirk)
 *   5. rtl8139_rx_aligned_len() — 4-byte alignment of a packet length
 *   6. RX header parsing logic  — simulate a crafted ring-buffer packet
 *
 * The hardware-dependent functions (rtl8139_init, rtl8139_send, rtl8139_recv,
 * rtl8139_get_mac) are guarded by #ifdef __is_kernel in rtl8139.c and are
 * NOT tested here.
 *
 * Build & run:  make  (from tests/)
 */

#include "framework.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <kernel/rtl8139.h>

/* ═══════════════════════════════════════════════════════════════
 * 1. Constants — verify register offsets and flag bit values
 * ═══════════════════════════════════════════════════════════════ */

static void test_register_offsets(void)
{
    ASSERT_EQ((unsigned)RTL8139_REG_MAC0,    0x00u, "MAC0 at offset 0x00");
    ASSERT_EQ((unsigned)RTL8139_REG_MAR0,    0x08u, "MAR0 at offset 0x08");
    ASSERT_EQ((unsigned)RTL8139_REG_TSD0,    0x10u, "TSD0 at offset 0x10");
    ASSERT_EQ((unsigned)RTL8139_REG_TSAD0,   0x20u, "TSAD0 at offset 0x20");
    ASSERT_EQ((unsigned)RTL8139_REG_RBSTART, 0x30u, "RBSTART at offset 0x30");
    ASSERT_EQ((unsigned)RTL8139_REG_CR,      0x37u, "CR at offset 0x37");
    ASSERT_EQ((unsigned)RTL8139_REG_CAPR,    0x38u, "CAPR at offset 0x38");
    ASSERT_EQ((unsigned)RTL8139_REG_CBR,     0x3Au, "CBR at offset 0x3A");
    ASSERT_EQ((unsigned)RTL8139_REG_IMR,     0x3Cu, "IMR at offset 0x3C");
    ASSERT_EQ((unsigned)RTL8139_REG_ISR,     0x3Eu, "ISR at offset 0x3E");
    ASSERT_EQ((unsigned)RTL8139_REG_TCR,     0x40u, "TCR at offset 0x40");
    ASSERT_EQ((unsigned)RTL8139_REG_RCR,     0x44u, "RCR at offset 0x44");
    ASSERT_EQ((unsigned)RTL8139_REG_CONFIG1, 0x52u, "CONFIG1 at offset 0x52");
}

static void test_cr_bits(void)
{
    ASSERT_EQ((unsigned)RTL8139_CR_RST,  0x10u, "RST is bit 4 (0x10)");
    ASSERT_EQ((unsigned)RTL8139_CR_RE,   0x08u, "RE is bit 3 (0x08)");
    ASSERT_EQ((unsigned)RTL8139_CR_TE,   0x04u, "TE is bit 2 (0x04)");
    ASSERT_EQ((unsigned)RTL8139_CR_BUFE, 0x01u, "BUFE is bit 0 (0x01)");

    /* No two CR bits overlap. */
    uint32_t all = RTL8139_CR_RST | RTL8139_CR_RE | RTL8139_CR_TE | RTL8139_CR_BUFE;
    ASSERT_EQ(all,
              RTL8139_CR_RST + RTL8139_CR_RE + RTL8139_CR_TE + RTL8139_CR_BUFE,
              "CR bits are non-overlapping");
}

static void test_isr_bits(void)
{
    ASSERT_EQ((unsigned)RTL8139_ISR_ROK,   0x0001u, "ROK is bit 0");
    ASSERT_EQ((unsigned)RTL8139_ISR_RER,   0x0002u, "RER is bit 1");
    ASSERT_EQ((unsigned)RTL8139_ISR_TOK,   0x0004u, "TOK is bit 2");
    ASSERT_EQ((unsigned)RTL8139_ISR_TER,   0x0008u, "TER is bit 3");
    ASSERT_EQ((unsigned)RTL8139_ISR_RXOVW, 0x0010u, "RXOVW is bit 4");
    ASSERT_EQ((unsigned)RTL8139_ISR_FOVW,  0x0040u, "FOVW is bit 6");
    ASSERT_EQ((unsigned)RTL8139_ISR_SERR,  0x8000u, "SERR is bit 15");
}

static void test_tsd_bits(void)
{
    ASSERT_EQ((unsigned)RTL8139_TSD_OWN, (1u << 13), "TSD_OWN is bit 13");
    ASSERT_EQ((unsigned)RTL8139_TSD_TUN, (1u << 14), "TSD_TUN is bit 14");
    ASSERT_EQ((unsigned)RTL8139_TSD_TOK, (1u << 15), "TSD_TOK is bit 15");
}

static void test_buffer_sizes(void)
{
    ASSERT_EQ((unsigned)RTL8139_RX_BUF_SIZE, 8192u,   "RX ring is 8 KiB");
    ASSERT_EQ((unsigned)RTL8139_RX_BUF_MASK, 8191u,   "mask = size - 1");
    ASSERT_EQ((unsigned)RTL8139_TX_SLOTS,    4u,       "4 TX descriptor slots");
    ASSERT_EQ((unsigned)RTL8139_RXHDR_SIZE,  4u,       "RX header is 4 bytes");
    ASSERT_EQ((unsigned)RTL8139_RX_CRC_LEN,  4u,       "CRC is 4 bytes");
    ASSERT_EQ((unsigned)RTL8139_MAC_LEN,     6u,       "MAC address is 6 bytes");

    /* TX buffer must accommodate a maximum Ethernet frame. */
    ASSERT(RTL8139_TX_BUF_SIZE >= RTL8139_MAX_ETH_FRAME,
           "TX buffer fits a max Ethernet frame");
}

/* ═══════════════════════════════════════════════════════════════
 * 2. rtl8139_bar0_iobase — I/O base extraction from BAR0
 * ═══════════════════════════════════════════════════════════════ */

static void test_bar0_iobase_basic(void)
{
    /* Typical QEMU RTL8139 BAR0: I/O base 0xC100, bit 0 set (I/O indicator). */
    ASSERT_EQ((unsigned)rtl8139_bar0_iobase(0xC101u), 0xC100u,
              "bar0=0xC101 → iobase=0xC100");
}

static void test_bar0_iobase_strips_low_bits(void)
{
    /* Bits 0 and 1 must always be cleared in the result. */
    /* 0x1235: bit 0 set → clear it → 0x1234 */
    ASSERT_EQ((unsigned)rtl8139_bar0_iobase(0x1235u), 0x1234u,
              "bit 0 set: cleared to give aligned base");
    ASSERT_EQ((unsigned)rtl8139_bar0_iobase(0x5000u), 0x5000u,
              "bar0 already aligned: unchanged");
    ASSERT_EQ((unsigned)rtl8139_bar0_iobase(0x5003u), 0x5000u,
              "bits 0+1 both set: both cleared");
}

static void test_bar0_iobase_zero(void)
{
    /* BAR0 = 0 means I/O base = 0 (card absent or unconfigured). */
    ASSERT_EQ((unsigned)rtl8139_bar0_iobase(0u), 0u,
              "bar0=0 → iobase=0");
}

/* ═══════════════════════════════════════════════════════════════
 * 3. rtl8139_rx_advance — RX ring position advancement
 * ═══════════════════════════════════════════════════════════════ */

static void test_rx_advance_simple(void)
{
    /* pos=0, 64-byte packet: header(4) + data(60) = 64 bytes; aligned = 64. */
    uint16_t next = rtl8139_rx_advance(0, 64);
    ASSERT_EQ((unsigned)next, 64u, "advance 64 from pos 0 → 64");
}

static void test_rx_advance_alignment(void)
{
    /* hdr_plus_pkt not already 4-byte aligned → rounds up. */
    /* pos=0, hdr_plus_pkt=61: (0+61+3)&~3 = 64 & 8191 = 64 */
    ASSERT_EQ((unsigned)rtl8139_rx_advance(0, 61), 64u,
              "61 bytes rounds up to 64");

    /* pos=0, hdr_plus_pkt=65: (0+65+3)&~3 = 68 & 8191 = 68 */
    ASSERT_EQ((unsigned)rtl8139_rx_advance(0, 65), 68u,
              "65 bytes rounds up to 68");

    /* pos=100, hdr_plus_pkt=10: (100+10+3)&~3 = 112 */
    ASSERT_EQ((unsigned)rtl8139_rx_advance(100, 10), 112u,
              "pos=100 + 10 bytes = 112");
}

static void test_rx_advance_wrap(void)
{
    /* pos near end of 8 KiB ring: wraps to start. */
    /* RTL8139_RX_BUF_SIZE = 8192.
     * pos=8180, hdr_plus_pkt=20: (8180+20+3)&~3 = 8203&~3 = 8200 & 8191 = 8 */
    ASSERT_EQ((unsigned)rtl8139_rx_advance(8180, 20), 8u,
              "wrap: pos=8180 + 20 → 8");

    /* Exact wrap: pos=8188, hdr_plus_pkt=4: (8188+4+3)&~3 = 8195&~3 = 8192 & 8191 = 0 */
    ASSERT_EQ((unsigned)rtl8139_rx_advance(8188, 4), 0u,
              "exact wrap to 0");

    /* Full ring advance wraps to 0. */
    ASSERT_EQ((unsigned)rtl8139_rx_advance(0, 8192), 0u,
              "advance full ring size wraps to 0");
}

static void test_rx_advance_mid_ring(void)
{
    /* pos=4096 (midpoint), 1520-byte packet (max typical frame).
     * hdr_plus_pkt = 4 + 1518 = 1522; aligned = 1524.
     * next = (4096 + 1522 + 3) & ~3 = 5621 & ~3 = 5620 & 8191 = 5620 */
    ASSERT_EQ((unsigned)rtl8139_rx_advance(4096, 1522), 5620u,
              "mid-ring advance: pos=4096 + 1522 → 5620");
}

/* ═══════════════════════════════════════════════════════════════
 * 4. rtl8139_capr_value — CAPR register encoding
 * ═══════════════════════════════════════════════════════════════ */

static void test_capr_value_zero(void)
{
    /* After init, rx_pos=0: CAPR = (0-16) = 0xFFF0.
     * This is the RTL8139 initial state after reset.   */
    ASSERT_EQ((unsigned)rtl8139_capr_value(0), 0xFFF0u,
              "rx_pos=0 → CAPR=0xFFF0 (hardware reset default)");
}

static void test_capr_value_non_zero(void)
{
    ASSERT_EQ((unsigned)rtl8139_capr_value(16),  0u,     "rx_pos=16 → CAPR=0");
    ASSERT_EQ((unsigned)rtl8139_capr_value(100), 84u,    "rx_pos=100 → CAPR=84");
    ASSERT_EQ((unsigned)rtl8139_capr_value(8192), 8176u, "rx_pos=8192 → CAPR=8176");
}

static void test_capr_value_wraps_16bit(void)
{
    /* 16-bit arithmetic: (0 - 16) wraps to 0xFFF0, not negative. */
    uint16_t v = rtl8139_capr_value(0);
    ASSERT(v == 0xFFF0u, "CAPR is a uint16_t — wraps on underflow");
}

/* ═══════════════════════════════════════════════════════════════
 * 5. rtl8139_rx_aligned_len — 4-byte alignment of packet length
 * ═══════════════════════════════════════════════════════════════ */

static void test_rx_aligned_len(void)
{
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(60),   60u,  "already aligned: 60");
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(61),   64u,  "61 → 64");
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(62),   64u,  "62 → 64");
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(63),   64u,  "63 → 64");
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(64),   64u,  "already aligned: 64");
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(100),  100u, "already aligned: 100");
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(1518), 1520u,"1518 → 1520");
    ASSERT_EQ((unsigned)rtl8139_rx_aligned_len(0),    0u,   "0 stays 0");
}

/* ═══════════════════════════════════════════════════════════════
 * 6. RX header parsing — simulate ring buffer with a crafted packet
 * ═══════════════════════════════════════════════════════════════ */

/*
 * Simulate what rtl8139_recv() does when reading the ring buffer.
 * We build a fake ring buffer with a known packet, then check that the
 * status and length extraction, data copy, and rx_pos advance are correct.
 */

/* Read a LE uint16 from a position in the ring buffer (with wrap). */
static uint16_t ring_read16(const uint8_t *ring, uint16_t pos)
{
    uint16_t p = pos & (uint16_t)RTL8139_RX_BUF_MASK;
    uint16_t lo = ring[p];
    uint16_t hi = ring[(p + 1u) & (uint16_t)RTL8139_RX_BUF_MASK];
    return (uint16_t)(lo | (hi << 8));
}

static void test_rx_header_parse_normal(void)
{
    /* Build a minimal ring buffer with a 14-byte Ethernet header "frame". */
    static uint8_t ring[RTL8139_RX_BUF_SIZE + RTL8139_RX_BUF_PAD];
    memset(ring, 0, sizeof(ring));

    /* pkt_len = 14 (data) + 4 (CRC) = 18; ROK = 1 */
    uint16_t pkt_len = 18u;
    ring[0] = 0x01u;                             /* status low byte: ROK  */
    ring[1] = 0x00u;                             /* status high byte      */
    ring[2] = (uint8_t)(pkt_len & 0xFF);         /* length low byte       */
    ring[3] = (uint8_t)(pkt_len >> 8);           /* length high byte      */
    /* Frame data at ring[4..17]: fill with 0x55. */
    for (int i = 0; i < 14; i++) ring[4 + i] = 0x55u;

    uint16_t rx_pos = 0;
    uint16_t status  = ring_read16(ring, rx_pos);
    uint16_t rlen    = ring_read16(ring, (uint16_t)(rx_pos + 2u));

    ASSERT((status & RTL8139_RXHDR_ROK), "crafted packet: ROK bit set");
    ASSERT_EQ((unsigned)rlen, 18u, "crafted packet: pkt_len = 18");

    uint16_t data_len = (uint16_t)(rlen - RTL8139_RX_CRC_LEN);
    ASSERT_EQ((unsigned)data_len, 14u, "data_len = pkt_len - CRC = 14");

    /* Verify the first data byte. */
    uint8_t first_byte = ring[(rx_pos + RTL8139_RXHDR_SIZE) & RTL8139_RX_BUF_MASK];
    ASSERT_EQ((unsigned)first_byte, 0x55u, "first data byte matches 0x55");

    /* Advance rx_pos: header(4) + pkt_len(18) = 22 → aligned to 24. */
    uint16_t new_pos = rtl8139_rx_advance(rx_pos,
                           (uint16_t)(RTL8139_RXHDR_SIZE + rlen));
    ASSERT_EQ((unsigned)new_pos, 24u, "rx_pos advances from 0 to 24");
}

static void test_rx_header_bad_status(void)
{
    /* A packet with ROK clear should be treated as an error. */
    static uint8_t ring[RTL8139_RX_BUF_SIZE];
    memset(ring, 0, sizeof(ring));

    ring[0] = 0x00u;   /* status: ROK=0 */
    ring[1] = 0x00u;
    ring[2] = 10u;     /* pkt_len = 10 */
    ring[3] = 0x00u;

    uint16_t rx_pos = 0;
    uint16_t status = ring_read16(ring, rx_pos);

    ASSERT(!(status & RTL8139_RXHDR_ROK),
           "bad packet: ROK bit is clear");
}

static void test_rx_header_wrap_around(void)
{
    /* Place a packet header that spans the ring boundary (byte 8191→0). */
    static uint8_t ring[RTL8139_RX_BUF_SIZE + RTL8139_RX_BUF_PAD];
    memset(ring, 0, sizeof(ring));

    /* Header at position 8191: bytes 8191, 0, 1, 2 */
    ring[8191] = 0x01u;  /* status low (ROK) */
    ring[0]    = 0x00u;  /* status high      */
    ring[1]    = 20u;    /* pkt_len low      */
    ring[2]    = 0x00u;  /* pkt_len high     */

    uint16_t rx_pos = 8191u;
    uint16_t status  = ring_read16(ring, rx_pos);
    uint16_t pkt_len = ring_read16(ring, (uint16_t)(rx_pos + 2u));

    ASSERT((status & RTL8139_RXHDR_ROK), "wrapped header: ROK set");
    ASSERT_EQ((unsigned)pkt_len, 20u, "wrapped header: pkt_len = 20");
}

/* ═══════════════════════════════════════════════════════════════
 * 7. Configuration register values
 * ═══════════════════════════════════════════════════════════════ */

static void test_rcr_value(void)
{
    /* RTL8139_RCR_VAL must include APM (bit 1), AB (bit 3), WRAP (bit 7). */
    ASSERT(RTL8139_RCR_VAL & (1u << 1),  "RCR has APM (accept physical match)");
    ASSERT(RTL8139_RCR_VAL & (1u << 3),  "RCR has AB (accept broadcast)");
    ASSERT(RTL8139_RCR_VAL & (1u << 7),  "RCR has WRAP (ring wrap)");

    /* RBLEN[12:11] = 00 → 8 KiB ring (bits must be 0). */
    ASSERT(!(RTL8139_RCR_VAL & (3u << 11)),
           "RCR RBLEN = 00 (8 KiB ring)");
}

static void test_tcr_value(void)
{
    /* TCR IFG[25:24] = 11 (standard 9.6 µs interframe gap). */
    ASSERT_EQ((unsigned)((RTL8139_TCR_VAL >> 24) & 0x3u), 3u,
              "TCR IFG = 11 (standard)");

    /* TCR MXDMA[10:8] = 111 (unlimited DMA burst). */
    ASSERT_EQ((unsigned)((RTL8139_TCR_VAL >> 8) & 0x7u), 7u,
              "TCR MXDMA = 111 (unlimited)");
}

/* ═══════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════ */

int main(void)
{
    RUN_SUITE(test_register_offsets);
    RUN_SUITE(test_cr_bits);
    RUN_SUITE(test_isr_bits);
    RUN_SUITE(test_tsd_bits);
    RUN_SUITE(test_buffer_sizes);

    RUN_SUITE(test_bar0_iobase_basic);
    RUN_SUITE(test_bar0_iobase_strips_low_bits);
    RUN_SUITE(test_bar0_iobase_zero);

    RUN_SUITE(test_rx_advance_simple);
    RUN_SUITE(test_rx_advance_alignment);
    RUN_SUITE(test_rx_advance_wrap);
    RUN_SUITE(test_rx_advance_mid_ring);

    RUN_SUITE(test_capr_value_zero);
    RUN_SUITE(test_capr_value_non_zero);
    RUN_SUITE(test_capr_value_wraps_16bit);

    RUN_SUITE(test_rx_aligned_len);

    RUN_SUITE(test_rx_header_parse_normal);
    RUN_SUITE(test_rx_header_bad_status);
    RUN_SUITE(test_rx_header_wrap_around);

    RUN_SUITE(test_rcr_value);
    RUN_SUITE(test_tcr_value);

    TEST_SUMMARY();
}
