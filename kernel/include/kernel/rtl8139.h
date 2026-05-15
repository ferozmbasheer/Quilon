/*
 * Quilon OS -- RTL8139 Network Card Driver (Section 10.2)
 *
 * The Realtek RTL8139 is a PCI Ethernet NIC emulated by QEMU with:
 *   -device rtl8139,netdev=net0 -netdev user,id=net0
 *
 * Register access uses the I/O port base read from PCI BAR0.
 * Bit 0 of BAR0 is the I/O-space indicator -- the actual base is BAR0 & ~3.
 *
 * Hardware flow
 * -------------
 *   TX: copy frame to a static DMA buffer, write physical addr to TSAD[slot],
 *       write frame length to TSD[slot], poll TSD[slot].TOK.
 *   RX: poll CR.BUFE (buffer-empty flag); read 4-byte header {status, length}
 *       from the ring buffer; copy payload; advance CAPR.
 *
 * The pure-C helpers (rtl8139_rx_advance, rtl8139_bar0_iobase,
 * rtl8139_capr_value, rtl8139_rx_aligned_len) are static inline so that
 * the host-side unit tests can exercise them without any x86 I/O.
 */

#ifndef _KERNEL_RTL8139_H
#define _KERNEL_RTL8139_H

#include <stdint.h>

/* -- I/O register offsets (relative to I/O base from BAR0) ---------------- */
#define RTL8139_REG_MAC0        0x00u   /* 6 bytes: MAC address                */
#define RTL8139_REG_MAR0        0x08u   /* 8 bytes: multicast filter           */
#define RTL8139_REG_TSD0        0x10u   /* 4 × 4 B TX status descriptors       */
#define RTL8139_REG_TSAD0       0x20u   /* 4 × 4 B TX start addresses          */
#define RTL8139_REG_RBSTART     0x30u   /* RX ring buffer start (physical addr) */
#define RTL8139_REG_CR          0x37u   /* Command register (1 byte)           */
#define RTL8139_REG_CAPR        0x38u   /* Current address of packet read (2 B)*/
#define RTL8139_REG_CBR         0x3Au   /* Current buffer address (2 B, r/o)   */
#define RTL8139_REG_IMR         0x3Cu   /* Interrupt mask register (2 bytes)   */
#define RTL8139_REG_ISR         0x3Eu   /* Interrupt status register (2 bytes) */
#define RTL8139_REG_TCR         0x40u   /* TX configuration register (4 bytes) */
#define RTL8139_REG_RCR         0x44u   /* RX configuration register (4 bytes) */
#define RTL8139_REG_CONFIG1     0x52u   /* Power management (1 byte)           */

/* -- Command register bits (RTL8139_REG_CR) ------------------------------- */
#define RTL8139_CR_RST          0x10u   /* Software reset (self-clearing)      */
#define RTL8139_CR_RE           0x08u   /* Receiver enable                     */
#define RTL8139_CR_TE           0x04u   /* Transmitter enable                  */
#define RTL8139_CR_BUFE         0x01u   /* RX buffer empty (read-only)         */

/* -- Interrupt status / mask bits ----------------------------------------- */
#define RTL8139_ISR_ROK         0x0001u  /* Receive OK                         */
#define RTL8139_ISR_RER         0x0002u  /* Receive error                      */
#define RTL8139_ISR_TOK         0x0004u  /* Transmit OK                        */
#define RTL8139_ISR_TER         0x0008u  /* Transmit error                     */
#define RTL8139_ISR_RXOVW       0x0010u  /* RX buffer overflow                 */
#define RTL8139_ISR_FOVW        0x0040u  /* RX FIFO overflow                   */
#define RTL8139_ISR_SERR        0x8000u  /* System error                       */

/* -- TX status descriptor bits (RTL8139_REG_TSD0 + slot*4) --------------- */
/* Bits [12:0]: byte count written before transmit.                          */
#define RTL8139_TSD_OWN         (1u << 13)  /* Card sets this on TX complete   */
#define RTL8139_TSD_TUN         (1u << 14)  /* TX FIFO underrun                */
#define RTL8139_TSD_TOK         (1u << 15)  /* TX OK                           */

/* -- RX packet header written by hardware at the ring read pointer --------- */
/* Layout: [u16 status][u16 pkt_len][frame data ...][pad to 4 B]             */
/* pkt_len includes the 4-byte CRC but NOT the 4-byte header.                */
#define RTL8139_RXHDR_ROK       0x0001u  /* ROK bit in the RX header status   */
#define RTL8139_RXHDR_SIZE      4u       /* Header bytes: 2 status + 2 length */
#define RTL8139_RX_CRC_LEN      4u       /* CRC length appended to each frame */

/* -- Buffer sizing --------------------------------------------------------- */
#define RTL8139_RX_BUF_SIZE     (8u * 1024u)   /* 8 KiB RX ring buffer        */
#define RTL8139_RX_BUF_MASK     (RTL8139_RX_BUF_SIZE - 1u)
#define RTL8139_RX_BUF_PAD      16u             /* wrap-around overflow area   */
#define RTL8139_TX_BUF_SIZE     1792u           /* max RTL8139 TX payload      */
#define RTL8139_TX_SLOTS        4u              /* round-robin TX descriptors  */
#define RTL8139_MAX_ETH_FRAME   1518u           /* max valid Ethernet frame    */
#define RTL8139_MAC_LEN         6u              /* MAC address bytes           */

/* -- Hardware configuration register values ------------------------------- */
/*
 * RCR: APM(1) | AB(3) | WRAP(7) | MXDMA=unlimited([10:8]=111) |
 *      RBLEN=8 KB([12:11]=00) | RXFTH=none([15:13]=111)
 * = 0x002 | 0x008 | 0x080 | 0x700 | 0x000 | 0xE000 = 0xE78A
 */
#define RTL8139_RCR_VAL         0x0000E78Au
/*
 * TCR: IFG=standard([25:24]=11) | MXDMA=unlimited([10:8]=111)
 * = 0x03000000 | 0x700 = 0x03000700
 */
#define RTL8139_TCR_VAL         0x03000700u

/* -- Pure-C helpers -- testable on the host without x86 I/O ---------------- */

/*
 * rtl8139_rx_advance -- advance the RX ring read pointer past one packet.
 *
 * hdr_plus_pkt = RXHDR_SIZE (4) + pkt_len_from_header (includes CRC).
 * The RTL8139 pads each entry to a 4-byte boundary.
 * Returns new rx_pos, wrapped within the 8 KiB ring.
 */
static inline uint16_t rtl8139_rx_advance(uint16_t rx_pos,
                                           uint16_t hdr_plus_pkt)
{
    uint32_t next = ((uint32_t)rx_pos + hdr_plus_pkt + 3u) & ~3u;
    return (uint16_t)(next & RTL8139_RX_BUF_MASK);
}

/*
 * rtl8139_bar0_iobase -- extract the I/O port base from a raw BAR0 value.
 *
 * PCI I/O BARs have bit 0 set as the I/O-space indicator (not part of the
 * address) and bit 1 reserved.  The actual base is BAR0 & ~3.
 */
static inline uint16_t rtl8139_bar0_iobase(uint32_t bar0)
{
    return (uint16_t)(bar0 & ~3u);
}

/*
 * rtl8139_capr_value -- CAPR register value for a given rx_pos.
 *
 * RTL8139 quirk: CAPR must be written as (current_read_pos - 16).
 * The hardware adds 16 back to get the actual next-packet address.
 */
static inline uint16_t rtl8139_capr_value(uint16_t rx_pos)
{
    return (uint16_t)((uint32_t)rx_pos - 16u);
}

/*
 * rtl8139_rx_aligned_len -- round a packet length up to 4-byte alignment.
 * Used when computing how many bytes in the ring a packet occupies.
 */
static inline uint16_t rtl8139_rx_aligned_len(uint16_t pkt_len)
{
    return (uint16_t)(((uint32_t)pkt_len + 3u) & ~3u);
}

/* -- Public kernel API ----------------------------------------------------- */

/*
 * rtl8139_init -- detect and initialise the RTL8139.
 *
 * Finds the card via pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139),
 * reads BAR0 for the I/O base, resets the card, configures RX/TX, enables
 * the receiver and transmitter, and reads the MAC address.
 *
 * Must call pci_enumerate() before rtl8139_init().
 * Returns 0 on success, -1 if the card is absent or reset timed out.
 */
int rtl8139_init(void);

/*
 * rtl8139_send -- transmit one raw Ethernet frame.
 *
 * data: pointer to frame bytes starting at the destination MAC address.
 * len: frame byte count; must be ≤ RTL8139_TX_BUF_SIZE (1792).
 * Returns 0 on success (TOK confirmed), -1 on error.
 */
int rtl8139_send(const void *data, uint16_t len);

/*
 * rtl8139_recv -- poll for a received Ethernet frame (no IRQ required).
 *
 * Copies up to maxlen bytes of the next received frame into buf.
 * Returns byte count (> 0) on success, 0 if no packet available,
 * -1 on hardware error (bad packet status).
 */
int rtl8139_recv(void *buf, uint16_t maxlen);

/*
 * rtl8139_get_mac -- copy the 6-byte hardware MAC address into mac[0..5].
 * Returns 0 on success, -1 if rtl8139_init() has not been called.
 */
int rtl8139_get_mac(uint8_t mac[6]);

/* Returns 1 if rtl8139_init() succeeded, 0 otherwise. */
int rtl8139_is_ready(void);

#endif /* _KERNEL_RTL8139_H */
