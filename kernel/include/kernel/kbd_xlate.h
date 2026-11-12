/*
 * Quilon OS -- PS/2 scancode (set 1) -> byte-stream translation (ROADMAP3 17.0.1)
 *
 * Pure logic, no hardware access: the keyboard IRQ feeds every raw byte read
 * from port 0x60 into kbd_translate(), which tracks modifier state and returns
 * the bytes a terminal program should read:
 *
 *   - printable keys, honouring Shift and Caps Lock (US layout)
 *   - Ctrl+letter -> 0x01..0x1A  (Ctrl-C = 0x03), Ctrl+[ \ ] -> 0x1B..0x1D
 *   - Alt+key     -> ESC, key    (xterm "meta sends escape")
 *   - navigation and function keys -> VT100/xterm escape sequences:
 *       Up ESC[A  Down ESC[B  Right ESC[C  Left ESC[D  Home ESC[H  End ESC[F
 *       Insert ESC[2~  Delete ESC[3~  PgUp ESC[5~  PgDn ESC[6~
 *       F1-F4 ESC O P..S   F5-F12 ESC[15~ ESC[17~ .. ESC[24~
 *     with Shift/Alt/Ctrl held, the xterm parameter forms (ROADMAP3 18.2):
 *       ESC[1;<m>A .. ESC[1;<m>S, ESC[<n>;<m>~, m = 1 + Shift 1 + Alt 2 +
 *       Ctrl 4 -- e.g. Shift-Up ESC[1;2A, Alt-F4 ESC[1;3S, Ctrl-Del ESC[3;5~
 *
 * Encoding special keys as escape sequences keeps the read()-a-byte-stream ABI
 * unchanged: terminals, the shells' line editors and a future vi all parse one
 * standard format.  Unit-tested on the host in tests/test_kbd.c.
 */

#ifndef _KERNEL_KBD_XLATE_H
#define _KERNEL_KBD_XLATE_H

#include <stdint.h>

#define KBD_SEQ_MAX 8   /* longest output for one scancode (ESC [ 2 4 ~ = 5) */

typedef struct {
    uint8_t shift_l, shift_r;   /* held */
    uint8_t ctrl_l, ctrl_r;     /* held */
    uint8_t alt_l, alt_r;       /* held */
    uint8_t caps;               /* toggled by Caps Lock presses */
    uint8_t e0;                 /* previous byte was the 0xE0 prefix */
    uint8_t e1_skip;            /* bytes of a Pause (0xE1 ...) sequence left */
} kbd_state_t;

void kbd_state_init(kbd_state_t *st);

/* Feed one raw scancode byte.  Writes 0..KBD_SEQ_MAX bytes to out and returns
 * the count (0 for prefixes, modifiers, releases and unmapped keys). */
int kbd_translate(kbd_state_t *st, uint8_t scancode, char *out);

#endif
