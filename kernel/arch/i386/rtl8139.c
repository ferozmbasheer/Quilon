/*
 * Quilon OS -- RTL8139 Network Card Driver (Section 10.2)
 *
 * Initialisation sequence
 * -----------------------
 * 1. pci_find_device(0x10EC, 0x8139) -- locate the card in the PCI table.
 * 2. pci_read BAR0 -> io_base (I/O BAR, bit 0 is the I/O indicator).
 * 3. Enable PCI I/O-space access (bit 0) and bus-master DMA (bit 2).
 * 4. Power on: CONFIG1 ← 0.
 * 5. Software reset: CR ← RST; poll until the bit self-clears (≤ 10 ms).
 * 6. RBSTART ← physical address of the static 8 KiB RX ring buffer.
 * 7. Clear and mask all interrupts (polling mode, no IRQ handler needed).
 * 8. TCR ← RTL8139_TCR_VAL (standard interframe gap, unlimited DMA burst).
 * 9. RCR ← RTL8139_RCR_VAL (accept broadcast + physical match, 8 KiB ring).
 * 10. CR ← RE | TE (enable receiver and transmitter).
 * 11. Read MAC address from the first 6 I/O bytes.
 * 12. Initialise CAPR to (0 - 16) so the first packet lands at ring offset 0.
 *
 * TX flow
 * -------
 * Frame is copied into tx_buf[slot] (static, always mapped into the kernel).
 * TSAD[slot] ← physical address of that buffer.
 * TSD[slot]  ← frame length (writing starts the DMA).
 * Poll TSD[slot] until TSD_TOK or TSD_TUN is set.
 * Advance tx_slot round-robin through 0–3.
 *
 * RX flow (polling)
 * -----------------
 * Check CR.BUFE: if set, no data in ring.
 * Read 4-byte header from rx_buf[rx_pos & mask]: {u16 status, u16 pkt_len}.
 *   pkt_len = data length + 4-byte CRC (does NOT include the 4-byte header).
 * Copy (pkt_len - 4) bytes of frame data to the caller's buffer.
 * Advance rx_pos: (rx_pos + header + pkt_len + 3) & ~3, wrapped in 8 KiB.
 * Write CAPR = rx_pos - 16 (RTL8139 hardware quirk).
 *
 * Physical addresses
 * ------------------
 * The kernel is linked at 0xC0100000 (virtual), loaded at 0x100000 (physical).
 * DMA buffers in kernel .bss must be given to the card as physical addresses:
 *   phys = (uint32_t)(uintptr_t)ptr - KERNEL_OFFSET   (KERNEL_OFFSET = 0xC0000000)
 */

#include <stdint.h>
#include <kernel/rtl8139.h>
#include <kernel/pci.h>
#include <kernel/paging.h>   /* KERNEL_OFFSET */
#include <stdio.h>

#ifdef __is_kernel

/* -- I/O port helpers ------------------------------------------------------ */

static inline void rtl_outb(uint16_t port, uint8_t val)
{
    asm volatile("outb %b0, %w1" :: "a"(val), "Nd"(port));
}

static inline void rtl_outw(uint16_t port, uint16_t val)
{
    asm volatile("outw %w0, %w1" :: "a"(val), "Nd"(port));
}

static inline void rtl_outl(uint16_t port, uint32_t val)
{
    asm volatile("outl %0, %w1" :: "a"(val), "Nd"(port));
}

static inline uint8_t rtl_inb(uint16_t port)
{
    uint8_t v;
    asm volatile("inb %w1, %b0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint16_t rtl_inw(uint16_t port)
{
    uint16_t v;
    asm volatile("inw %w1, %w0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t rtl_inl(uint16_t port)
{
    uint32_t v;
    asm volatile("inl %w1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* -- DMA buffers (kernel .bss -- physical addr = virt - KERNEL_OFFSET) ----- */

static uint8_t rx_buf[RTL8139_RX_BUF_SIZE + RTL8139_RX_BUF_PAD]
    __attribute__((aligned(8)));

static uint8_t tx_buf[RTL8139_TX_SLOTS][RTL8139_TX_BUF_SIZE]
    __attribute__((aligned(4)));

/* -- Driver state ---------------------------------------------------------- */

static uint16_t io_base  = 0;
static int      tx_slot  = 0;
static uint16_t rx_pos   = 0;
static int      nic_ready = 0;
static uint8_t  mac_addr[RTL8139_MAC_LEN];

/* -- Virtual -> physical address conversion --------------------------------- */

static inline uint32_t to_phys(const void *virt)
{
    return (uint32_t)(uintptr_t)virt - KERNEL_OFFSET;
}

/* -- Public API ------------------------------------------------------------ */

int rtl8139_init(void)
{
    const pci_device_t *dev =
        pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139);
    if (!dev) return -1;

    /* Read BAR0; bit 0 is the I/O-space indicator, not part of the address. */
    uint32_t bar0 = pci_read(dev->bus, dev->slot, dev->func, PCI_OFF_BAR0);
    io_base = rtl8139_bar0_iobase(bar0);
    if (io_base == 0) return -1;

    /* Enable PCI I/O space (bit 0) and bus-master DMA (bit 2). */
    uint32_t cmd = pci_read(dev->bus, dev->slot, dev->func, PCI_OFF_COMMAND);
    pci_write(dev->bus, dev->slot, dev->func, PCI_OFF_COMMAND, cmd | 0x05u);

    /* Power on. */
    rtl_outb((uint16_t)(io_base + RTL8139_REG_CONFIG1), 0x00);

    /* Software reset: set RST, wait for self-clear. */
    rtl_outb((uint16_t)(io_base + RTL8139_REG_CR), RTL8139_CR_RST);
    int timeout = 100000;
    while ((rtl_inb((uint16_t)(io_base + RTL8139_REG_CR)) & RTL8139_CR_RST)
           && --timeout)
        asm volatile("pause");
    if (timeout == 0) return -1;

    /* Point the card at our RX ring buffer (physical address). */
    rtl_outl((uint16_t)(io_base + RTL8139_REG_RBSTART), to_phys(rx_buf));

    /* Clear all pending interrupts, then mask them all (polling mode). */
    rtl_outw((uint16_t)(io_base + RTL8139_REG_ISR), 0xFFFFu);
    rtl_outw((uint16_t)(io_base + RTL8139_REG_IMR), 0x0000u);

    /* TX and RX configuration. */
    rtl_outl((uint16_t)(io_base + RTL8139_REG_TCR), RTL8139_TCR_VAL);
    rtl_outl((uint16_t)(io_base + RTL8139_REG_RCR), RTL8139_RCR_VAL);

    /* Enable RX and TX. */
    rtl_outb((uint16_t)(io_base + RTL8139_REG_CR),
             RTL8139_CR_RE | RTL8139_CR_TE);

    /* Read the 6-byte MAC address from I/O bytes 0–5. */
    for (int i = 0; i < (int)RTL8139_MAC_LEN; i++)
        mac_addr[i] = rtl_inb((uint16_t)(io_base + RTL8139_REG_MAC0 + (uint16_t)i));

    /* Initialise read pointer: CAPR = (0 - 16), so first packet lands at 0. */
    rx_pos = 0;
    rtl_outw((uint16_t)(io_base + RTL8139_REG_CAPR), rtl8139_capr_value(0));

    nic_ready = 1;
    return 0;
}

int rtl8139_send(const void *data, uint16_t len)
{
    if (!nic_ready || !data) return -1;
    if (len == 0 || len > RTL8139_TX_BUF_SIZE) return -1;

    /* Copy the frame into the TX buffer for this slot. */
    const uint8_t *src = (const uint8_t *)data;
    uint8_t       *dst = tx_buf[tx_slot];
    for (uint16_t i = 0; i < len; i++)
        dst[i] = src[i];

    /* Write the physical address of the buffer. */
    rtl_outl((uint16_t)(io_base + RTL8139_REG_TSAD0 + (uint16_t)(tx_slot * 4)),
             to_phys(dst));

    /* Writing TSD (frame length in bits [12:0]) starts the DMA transfer.
     * The hardware sets TSD_OWN and then TSD_TOK when transmission completes. */
    rtl_outl((uint16_t)(io_base + RTL8139_REG_TSD0 + (uint16_t)(tx_slot * 4)),
             (uint32_t)(len & 0x1FFFu));

    /* Poll for TOK (success) or TUN (underrun). */
    int timeout = 10000000;
    uint32_t tsd;
    do {
        tsd = rtl_inl((uint16_t)(io_base + RTL8139_REG_TSD0 +
                                  (uint16_t)(tx_slot * 4)));
        if (tsd & (RTL8139_TSD_TOK | RTL8139_TSD_TUN)) break;
        asm volatile("pause");
    } while (--timeout);

    tx_slot = (tx_slot + 1) % (int)RTL8139_TX_SLOTS;

    if (timeout == 0)              return -1;
    if (tsd & RTL8139_TSD_TUN)    return -1;
    return 0;
}

int rtl8139_recv(void *buf, uint16_t maxlen)
{
    if (!nic_ready || !buf) return -1;

    /* CR.BUFE is set when the ring buffer is empty. */
    if (rtl_inb((uint16_t)(io_base + RTL8139_REG_CR)) & RTL8139_CR_BUFE)
        return 0;

    /* Read the 4-byte hardware header at the current ring position.
     * Access through the mask to handle wrap-around transparently.  */
    uint16_t p = rx_pos & (uint16_t)RTL8139_RX_BUF_MASK;

    uint16_t status  = (uint16_t)( rx_buf[p]
                                  | ((uint16_t)rx_buf[(p + 1u) & RTL8139_RX_BUF_MASK] << 8));
    uint16_t pkt_len = (uint16_t)( rx_buf[(p + 2u) & RTL8139_RX_BUF_MASK]
                                  | ((uint16_t)rx_buf[(p + 3u) & RTL8139_RX_BUF_MASK] << 8));

    if (!(status & RTL8139_RXHDR_ROK)) {
        /* Discard bad packet: advance past it and report error. */
        rx_pos = rtl8139_rx_advance(rx_pos,
                     (uint16_t)(RTL8139_RXHDR_SIZE + pkt_len));
        rtl_outw((uint16_t)(io_base + RTL8139_REG_CAPR),
                 rtl8139_capr_value(rx_pos));
        rtl_outw((uint16_t)(io_base + RTL8139_REG_ISR), RTL8139_ISR_RER);
        return -1;
    }

    /* Data length = pkt_len - 4 (strip CRC). */
    uint16_t data_len = (pkt_len > RTL8139_RX_CRC_LEN)
                        ? (uint16_t)(pkt_len - RTL8139_RX_CRC_LEN)
                        : 0u;
    if (data_len > maxlen) data_len = maxlen;

    /* Copy payload, handling ring wrap-around byte by byte. */
    uint8_t *dst = (uint8_t *)buf;
    for (uint16_t i = 0; i < data_len; i++)
        dst[i] = rx_buf[(p + RTL8139_RXHDR_SIZE + i) & RTL8139_RX_BUF_MASK];

    /* Advance the ring read pointer and update CAPR. */
    rx_pos = rtl8139_rx_advance(rx_pos,
                 (uint16_t)(RTL8139_RXHDR_SIZE + pkt_len));
    rtl_outw((uint16_t)(io_base + RTL8139_REG_CAPR),
             rtl8139_capr_value(rx_pos));

    /* Acknowledge ROK. */
    rtl_outw((uint16_t)(io_base + RTL8139_REG_ISR), RTL8139_ISR_ROK);

    return (int)data_len;
}

int rtl8139_get_mac(uint8_t mac[6])
{
    if (!nic_ready) return -1;
    for (int i = 0; i < (int)RTL8139_MAC_LEN; i++)
        mac[i] = mac_addr[i];
    return 0;
}

int rtl8139_is_ready(void)
{
    return nic_ready;
}

#endif /* __is_kernel */
