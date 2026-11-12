/*
 * Quilon OS -- PS/2 Mouse Driver  (section 13)
 *
 * The PS/2 controller (Intel 8042) manages two ports:
 *   Port 1 (IRQ1)  -- keyboard
 *   Port 2 (IRQ12) -- auxiliary device (mouse)
 *
 * The mouse sends 3-byte packets on each movement or button event.
 * Packet format:
 *   Byte 0: bit 0 = left button, bit 1 = right button, bit 2 = middle button
 *           bit 3 = always 1 (start bit), bit 4 = X sign, bit 5 = Y sign
 *           bits 6-7 = overflow flags (ignore if set)
 *   Byte 1: X delta (magnitude; sign from byte 0 bit 4)
 *   Byte 2: Y delta (magnitude; sign from byte 0 bit 5; positive = up on screen)
 *
 * The driver exposes:
 *   mouse_initialize()     -- enable the auxiliary port and data reporting
 *   mouse_get_x/y()        -- current cursor position in pixels
 *   mouse_get_buttons()    -- bitmask of pressed buttons (bits 0-2)
 *   mouse_irq_handler()    -- called by irq12_handler() in interrupts.c
 *
 * Pure-C helpers (mouse_packet_dx, mouse_packet_dy, mouse_clamp) are
 * defined as static inline in this header so they are testable on the host
 * without needing __is_kernel code.
 */

#ifndef _KERNEL_MOUSE_H
#define _KERNEL_MOUSE_H

#include <stdint.h>

/* -- Packet flag bits (byte 0) ------------------------------------------- */
#define MOUSE_BTN_LEFT   0x01u
#define MOUSE_BTN_RIGHT  0x02u
#define MOUSE_BTN_MIDDLE 0x04u
#define MOUSE_START_BIT  0x08u   /* always 1 in a valid packet */
#define MOUSE_X_SIGN     0x10u   /* set if X delta is negative */
#define MOUSE_Y_SIGN     0x20u   /* set if Y delta is negative */
#define MOUSE_X_OVERFLOW 0x40u
#define MOUSE_Y_OVERFLOW 0x80u

/* -- Cursor size ---------------------------------------------------------- */
#define MOUSE_CURSOR_W  8u
#define MOUSE_CURSOR_H  8u

/* -- Pure-C helpers (testable on host without __is_kernel) ---------------- */

/*
 * mouse_packet_dx -- sign-extend the X delta from a 3-byte mouse packet.
 * pkt[0] bit 4 = sign, pkt[1] = magnitude.
 */
static inline int mouse_packet_dx(const uint8_t pkt[3])
{
    int dx = (int)(uint8_t)pkt[1];
    if (pkt[0] & MOUSE_X_SIGN) dx -= 256;
    return dx;
}

/*
 * mouse_packet_dy -- sign-extend the Y delta.
 * Note: PS/2 Y is inverted (positive = up); the driver negates it so
 * positive Y means "cursor moves down" (screen coordinates).
 */
static inline int mouse_packet_dy(const uint8_t pkt[3])
{
    int dy = (int)(uint8_t)pkt[2];
    if (pkt[0] & MOUSE_Y_SIGN) dy -= 256;
    return -dy;   /* invert: PS/2 positive = up, screen positive = down */
}

/*
 * mouse_clamp -- clamp v to [lo, hi].
 */
static inline int mouse_clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* -- Kernel-only API ------------------------------------------------------ */

/*
 * IntelliMouse wheel (ROADMAP3 18.2).  Setting the sample rate to 200, 100,
 * 80 in a row is the "knock" that switches a wheel mouse to device ID 3,
 * whose packets are 4 bytes; byte 3 holds the wheel movement Z (we use its
 * low 4 bits, two's complement -- correct for ID 3 and ID 4 mice alike).
 * Positive Z = wheel turned towards the user (scroll down).
 */
#define MOUSE_ID_WHEEL 3u

static inline int mouse_packet_dz(uint8_t b)
{
    int z = b & 0x0F;
    return (z & 0x08) ? z - 16 : z;
}

/* Button edges between two packets' button states (pure, host-tested). */
static inline void mouse_button_edges(uint8_t before, uint8_t after,
                                      uint8_t *pressed, uint8_t *released)
{
    *pressed  |= (uint8_t)(after & ~before);
    *released |= (uint8_t)(before & ~after);
}

#ifdef __is_kernel

/*
 * mouse_initialize -- enable the PS/2 auxiliary port and start the mouse.
 *
 * Sequence:
 *   1. Enable auxiliary device port (cmd 0xA8 to 0x64).
 *   2. Enable IRQ12 in the 8042 command byte (read 0x20, set bit 1, write 0x60).
 *   3. Send mouse command 0xF6 (set defaults) then 0xF4 (enable reporting).
 *
 * Must be called after idt_initialize() because IRQ12 (vector 44) must be
 * registered before the first mouse interrupt fires.
 */
void mouse_initialize(void);

/*
 * mouse_irq_handler -- process one byte from the 8042 data port.
 *
 * Called by irq12_handler() in interrupts.c.  Accumulates bytes into a
 * 3-byte packet; on the third byte, updates mouse_x / mouse_y / mouse_buttons
 * and redraws the cursor on the VBE framebuffer (if active).
 */
void mouse_irq_handler(void);

/* Query the current mouse state (valid after mouse_initialize). */
int mouse_get_x(void);
int mouse_get_y(void);
uint8_t mouse_get_buttons(void);
/* Number of complete packets received so far (wraps).  SYS_WAIT_INPUT
 * compares it with the PCB's mouse_seq_seen.  ROADMAP3 18.1. */
uint32_t mouse_get_seq(void);
/* Take (and clear) the button edges and wheel notches latched since the last
 * call -- SYS_MOUSE_READ.  IRQ-safe. */
void mouse_take_events(uint8_t *pressed, uint8_t *released, int *wheel);
/* 1 if a wheel (IntelliMouse) protocol was negotiated at init. */
int  mouse_has_wheel(void);

/*
 * mouse_cursor_invalidate -- erase the on-screen cursor from the shadow buffer.
 *
 * Restores the pixels that were saved under the cursor, marks cursor_drawn=0,
 * and calls vbe_dirty_rows so vbe_flush picks up the erase.  Call this before
 * any operation that rewrites shadow_buf in bulk (e.g. VBE terminal scroll),
 * otherwise cursor_saved[] becomes stale and cursor_restore will write old
 * pixels over the new content on the next mouse event.
 */
void mouse_cursor_invalidate(void);

#endif /* __is_kernel */

#endif /* _KERNEL_MOUSE_H */
