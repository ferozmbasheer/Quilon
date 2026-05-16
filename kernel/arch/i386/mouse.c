/*
 * Quilon OS -- PS/2 Mouse Driver  (section 13)
 *
 * Drives the PS/2 auxiliary port (mouse) on the Intel 8042 keyboard
 * controller.  The mouse sends 3-byte packets on IRQ12 (vector 44).
 *
 * Initialization sequence:
 *   0xA8 -> 0x64   enable auxiliary port
 *   0x20 -> 0x64   read command byte
 *   (OR in bit 1)  enable aux interrupt
 *   0x60 <- 0x64   write command byte back
 *   0xD4 -> 0x64   route next byte to mouse
 *   0xF6 -> 0x60   set defaults
 *   0xD4 -> 0x64   route next byte to mouse
 *   0xF4 -> 0x60   enable data reporting
 *
 * Cursor rendering:
 *   The driver paints a small cross-hair cursor into the VBE shadow buffer
 *   using save/restore to avoid permanent pixel corruption.  On each packet
 *   the old cursor pixels are restored and the new cursor is painted.
 *   vbe_flush() is called after the repaint so the frame reaches the screen.
 */

#include <stdint.h>
#include <kernel/mouse.h>
#include <kernel/vbe.h>

#ifdef __is_kernel

/* -- Port I/O helpers (defined in boot.S) -------------------------------- */
extern void    outb(unsigned short port, unsigned char data);
extern char    inb(unsigned short port);

#define PS2_DATA    0x60u
#define PS2_CMD     0x64u

/* -- Module state -------------------------------------------------------- */

static int     mouse_x       = 0;
static int     mouse_y       = 0;
static uint8_t mouse_buttons = 0;

static uint8_t  packet[3];
static uint8_t  packet_cycle = 0;   /* which byte of the 3-byte packet */

/*
 * Saved pixels under the cursor in the shadow buffer (BGRX, 32 bpp).
 * Only used when VBE is active and bpp == 32.
 * Size: MOUSE_CURSOR_W × MOUSE_CURSOR_H × 4 bytes = 256 bytes.
 */
static uint32_t cursor_saved[MOUSE_CURSOR_W * MOUSE_CURSOR_H];
static int      cursor_drawn  = 0;   /* 1 if cursor pixels are currently painted */
static int      cursor_prev_x = 0;
static int      cursor_prev_y = 0;

/* -- PS/2 helpers -------------------------------------------------------- */

/* Wait until the 8042 output buffer is full (bit 0 of status). */
static inline void ps2_wait_output(void)
{
    int timeout = 100000;
    while (timeout-- > 0 && !(inb(PS2_CMD) & 0x01u));
}

/* Wait until the 8042 input buffer is empty (bit 1 of status == 0). */
static inline void ps2_wait_input(void)
{
    int timeout = 100000;
    while (timeout-- > 0 && (inb(PS2_CMD) & 0x02u));
}

/* Send a command byte to the 8042 controller (port 0x64). */
static inline void ps2_send_cmd(uint8_t cmd)
{
    ps2_wait_input();
    outb(PS2_CMD, cmd);
}

/* Send a data byte to port 0x60. */
static inline void ps2_send_data(uint8_t data)
{
    ps2_wait_input();
    outb(PS2_DATA, data);
}

/* Read one byte from port 0x60. */
static inline uint8_t ps2_read_data(void)
{
    ps2_wait_output();
    return (uint8_t)inb(PS2_DATA);
}

/* Send a byte to the mouse (via the 0xD4 routing command). */
static inline void mouse_send(uint8_t cmd)
{
    ps2_send_cmd(0xD4u);   /* route next byte to auxiliary port */
    ps2_send_data(cmd);
    ps2_read_data();        /* discard ACK (0xFA) */
}

/* -- Cursor rendering ---------------------------------------------------- */

/*
 * Cursor shape: 8×8 cross-hair.  Bit 7 = leftmost pixel.
 * We draw it with a bright white centre and a dark border for visibility.
 */
static const uint8_t cursor_shape[MOUSE_CURSOR_H] = {
    0b00011000,   /* row 0: two centre pixels */
    0b00011000,
    0b00011000,
    0b11111111,   /* row 3: full horizontal bar */
    0b11111111,   /* row 4: full horizontal bar */
    0b00011000,
    0b00011000,
    0b00011000,   /* row 7 */
};

#define CURSOR_FG  0x00FFFFFFu   /* white */
#define CURSOR_BG  0x00000000u   /* black (transparent, not painted) */

static void cursor_save(int x, int y)
{
    if (!vbe_active()) return;
    const vbe_info_t *info = vbe_get_info();
    if (info->bpp != 32) return;

    for (uint32_t row = 0; row < MOUSE_CURSOR_H; row++) {
        int py = y + (int)row;
        if (py < 0 || (uint32_t)py >= info->height) {
            for (uint32_t col = 0; col < MOUSE_CURSOR_W; col++)
                cursor_saved[row * MOUSE_CURSOR_W + col] = 0;
            continue;
        }
        for (uint32_t col = 0; col < MOUSE_CURSOR_W; col++) {
            int px = x + (int)col;
            if (px < 0 || (uint32_t)px >= info->width) {
                cursor_saved[row * MOUSE_CURSOR_W + col] = 0;
                continue;
            }
            /* Read from shadow buffer via VBE_SHADOW_VBASE mapping */
            uint8_t *shadow = (uint8_t *)VBE_SHADOW_VBASE;
            uint32_t off = (uint32_t)py * info->pitch + (uint32_t)px * 4u;
            cursor_saved[row * MOUSE_CURSOR_W + col] = *(uint32_t *)(shadow + off);
        }
    }
}

static void cursor_restore(int x, int y)
{
    if (!vbe_active()) return;
    const vbe_info_t *info = vbe_get_info();
    if (info->bpp != 32) return;

    for (uint32_t row = 0; row < MOUSE_CURSOR_H; row++) {
        int py = y + (int)row;
        if (py < 0 || (uint32_t)py >= info->height) continue;
        for (uint32_t col = 0; col < MOUSE_CURSOR_W; col++) {
            int px = x + (int)col;
            if (px < 0 || (uint32_t)px >= info->width) continue;
            uint8_t *shadow = (uint8_t *)VBE_SHADOW_VBASE;
            uint32_t off = (uint32_t)py * info->pitch + (uint32_t)px * 4u;
            *(uint32_t *)(shadow + off) = cursor_saved[row * MOUSE_CURSOR_W + col];
        }
    }
}

static void cursor_paint(int x, int y)
{
    if (!vbe_active()) return;
    const vbe_info_t *info = vbe_get_info();
    if (info->bpp != 32) return;   /* save/restore don't support 24-bpp */
    for (uint32_t row = 0; row < MOUSE_CURSOR_H; row++) {
        for (uint32_t col = 0; col < MOUSE_CURSOR_W; col++) {
            if (cursor_shape[row] & (0x80u >> col))
                vbe_draw_pixel((uint32_t)(x + (int)col),
                               (uint32_t)(y + (int)row),
                               CURSOR_FG);
        }
    }
}

/* Move the on-screen cursor from (cursor_prev_x, cursor_prev_y) to (nx, ny). */
static void cursor_move(int nx, int ny)
{
    if (!vbe_active()) return;

    if (cursor_drawn) {
        /* Write saved background pixels back into shadow_buf at the old
         * position (direct write — fast, no dirty overhead per pixel), then
         * tell the dirty tracker about those rows so vbe_flush copies them to
         * the hardware framebuffer and erases the old cursor image there. */
        cursor_restore(cursor_prev_x, cursor_prev_y);
        vbe_dirty_rows((uint32_t)cursor_prev_y, MOUSE_CURSOR_H);
    }

    /* Save pixels at the new position, then draw the cursor. */
    cursor_save(nx, ny);
    cursor_paint(nx, ny);

    cursor_drawn  = 1;
    cursor_prev_x = nx;
    cursor_prev_y = ny;

    /* Flush the changed rows to the hardware framebuffer. */
    vbe_flush();
}

/* Repaint the cursor at its current position (called by the post-scroll hook). */
static void mouse_cursor_redraw(void)
{
    cursor_move(mouse_x, mouse_y);
}

/* -- Public API ---------------------------------------------------------- */

void mouse_initialize(void)
{
    /* 1. Enable the auxiliary PS/2 port. */
    ps2_send_cmd(0xA8u);

    /* 2. Read the 8042 command byte, set bit 1 (enable IRQ12), write back. */
    ps2_send_cmd(0x20u);          /* request command byte */
    uint8_t cmd_byte = ps2_read_data();
    cmd_byte |= 0x02u;            /* bit 1 = auxiliary interrupt enable */
    cmd_byte &= ~0x20u;           /* bit 5 = clear "auxiliary port disabled" flag */
    ps2_send_cmd(0x60u);          /* prepare to write command byte */
    ps2_send_data(cmd_byte);

    /* 3. Set mouse defaults (0xF6) then enable data reporting (0xF4). */
    mouse_send(0xF6u);   /* set defaults */
    mouse_send(0xF4u);   /* enable reporting */

    /* 4. Initialise position to centre of screen (if VBE active). */
    if (vbe_active()) {
        const vbe_info_t *info = vbe_get_info();
        mouse_x = (int)(info->width  / 2u);
        mouse_y = (int)(info->height / 2u);
    }

    packet_cycle  = 0;
    cursor_drawn  = 0;
    cursor_prev_x = mouse_x;
    cursor_prev_y = mouse_y;

    /* Erase cursor before the scroll memmove; repaint it immediately after. */
    vbe_set_scroll_hook(mouse_cursor_invalidate);
    vbe_set_post_scroll_hook(mouse_cursor_redraw);

    /* Draw the initial cursor. */
    cursor_move(mouse_x, mouse_y);
}

void mouse_irq_handler(void)
{
    uint8_t byte = (uint8_t)inb(PS2_DATA);

    /* Synchronise: byte 0 must always have bit 3 set.  If we lose sync,
     * wait for the next byte that looks like a valid packet start. */
    if (packet_cycle == 0 && !(byte & MOUSE_START_BIT))
        return;

    packet[packet_cycle++] = byte;

    if (packet_cycle < 3)
        return;   /* accumulate the full 3-byte packet */

    packet_cycle = 0;

    /* Saturate overflow packets to the 9-bit extremes rather than discarding
     * them.  QEMU (and most hardware) clamps bytes 1/2 to ±255/256 when the
     * overflow bits are set, so the sign bit correctly indicates direction. */
    int dx, dy;
    if (packet[0] & MOUSE_X_OVERFLOW)
        dx = (packet[0] & MOUSE_X_SIGN) ? -256 : 255;
    else
        dx = mouse_packet_dx(packet);

    if (packet[0] & MOUSE_Y_OVERFLOW)
        /* mirror mouse_packet_dy's screen-space inversion: sign set = downward
         * physical = positive screen Y, so raw = -256 → dy = +256. */
        dy = (packet[0] & MOUSE_Y_SIGN) ? 256 : -255;
    else
        dy = mouse_packet_dy(packet);
    mouse_buttons = packet[0] & (MOUSE_BTN_LEFT | MOUSE_BTN_RIGHT | MOUSE_BTN_MIDDLE);

    /* Clamp cursor to screen bounds. */
    if (vbe_active()) {
        const vbe_info_t *info = vbe_get_info();
        mouse_x = mouse_clamp(mouse_x + dx, 0, (int)info->width  - 1);
        mouse_y = mouse_clamp(mouse_y + dy, 0, (int)info->height - 1);
    } else {
        mouse_x += dx;
        mouse_y += dy;
    }

    cursor_move(mouse_x, mouse_y);
}

int     mouse_get_x(void)       { return mouse_x; }
int     mouse_get_y(void)       { return mouse_y; }
uint8_t mouse_get_buttons(void) { return mouse_buttons; }

void mouse_cursor_invalidate(void)
{
    /* Erase the cursor from shadow_buf BEFORE the scroll memmove so that the
     * cursor shape is not carried up the screen with the shifted content.
     * dirty_mark_all() fires immediately after this hook, so we do not need
     * to call vbe_dirty_rows here — the full-screen flush will cover it. */
    if (cursor_drawn)
        cursor_restore(cursor_prev_x, cursor_prev_y);
    cursor_drawn = 0;
    for (uint32_t i = 0; i < MOUSE_CURSOR_W * MOUSE_CURSOR_H; i++)
        cursor_saved[i] = 0;
}

#endif /* __is_kernel */
