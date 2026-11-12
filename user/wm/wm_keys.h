/*
 * Quilon OS -- WM key decoding (ROADMAP3 18.2)
 *
 * The keyboard reaches the WM as the byte stream the kernel's kbd_xlate
 * produces (ROADMAP3 17.0.1): printable characters, control bytes
 * (Ctrl-A..Ctrl-Z = 0x01..0x1A), ESC-prefixed characters for Alt, and
 * VT100/xterm escape sequences for the special keys:
 *
 *   Up ESC[A  Down ESC[B  Right ESC[C  Left ESC[D  Home ESC[H  End ESC[F
 *   Insert ESC[2~  Delete ESC[3~  PgUp ESC[5~  PgDn ESC[6~
 *   F1-F4 ESC O P..S   F5-F12 ESC[15~ ESC[17~ .. ESC[24~
 *
 * and, with Shift/Alt/Ctrl held, the xterm "modifyOtherKeys"-style
 * parameter forms ESC[1;<m>A, ESC[<n>;<m>~, ESC[1;<m>P where
 * m = 1 + (Shift ? 1) + (Alt ? 2) + (Ctrl ? 4).
 *
 * wm_key_decode() turns the front of such a buffer into one key event: a
 * keycode, modifier bits, the ASCII byte (0 for special keys) and the raw
 * bytes it came from, which a terminal forwards to its shell unchanged.
 *
 * The kernel queues a key's whole sequence from one keyboard IRQ, and the WM
 * drains everything available before decoding, so a sequence is never split
 * across reads: an ESC with nothing after it is the Esc key itself.
 *
 * Pure C, host-tested in tests/test_wm.c.
 */
#ifndef _USER_WM_WM_KEYS_H
#define _USER_WM_WM_KEYS_H

#include <stdint.h>

/* Modifier bits (wm_key_t.mods / wm_event_t.mods). */
#define WM_MOD_SHIFT  0x01u
#define WM_MOD_ALT    0x02u
#define WM_MOD_CTRL   0x04u

/* Keycodes.  Printable keys use their (lower-case, for letters) ASCII value;
 * Ctrl+letter uses the letter.  Special keys are >= 0x100. */
enum {
    WM_KEY_NONE      = 0,
    WM_KEY_BACKSPACE = 0x08,
    WM_KEY_TAB       = 0x09,
    WM_KEY_ENTER     = 0x0A,
    WM_KEY_ESC       = 0x1B,
    WM_KEY_UP        = 0x100,
    WM_KEY_DOWN,
    WM_KEY_LEFT,
    WM_KEY_RIGHT,
    WM_KEY_HOME,
    WM_KEY_END,
    WM_KEY_INSERT,
    WM_KEY_DELETE,
    WM_KEY_PGUP,
    WM_KEY_PGDN,
    WM_KEY_F1,                   /* F1..F12 are consecutive */
    WM_KEY_F12       = WM_KEY_F1 + 11,
    WM_KEY_UNKNOWN   = 0x1FF     /* a well-formed sequence we don't name */
};

#define WM_KEY_SEQ_MAX 8

typedef struct {
    int     keycode;
    uint8_t mods;
    char    ascii;                  /* the character, or 0 for special keys */
    char    seq[WM_KEY_SEQ_MAX];    /* the raw bytes this key arrived as    */
    int     seq_len;
} wm_key_t;

/* xterm modifier parameter m (1 = none) -> WM_MOD_* bits. */
static inline uint8_t wm_key_mods_from_param(int m)
{
    if (m < 2 || m > 8) return 0;
    m -= 1;
    return (uint8_t)(((m & 1) ? WM_MOD_SHIFT : 0) |
                     ((m & 2) ? WM_MOD_ALT   : 0) |
                     ((m & 4) ? WM_MOD_CTRL  : 0));
}

/* CSI final letter (A-D, H, F, P-S) -> keycode, or WM_KEY_UNKNOWN. */
static inline int wm_key_from_final(char f)
{
    switch (f) {
    case 'A': return WM_KEY_UP;
    case 'B': return WM_KEY_DOWN;
    case 'C': return WM_KEY_RIGHT;
    case 'D': return WM_KEY_LEFT;
    case 'H': return WM_KEY_HOME;
    case 'F': return WM_KEY_END;
    case 'P': return WM_KEY_F1;
    case 'Q': return WM_KEY_F1 + 1;
    case 'R': return WM_KEY_F1 + 2;
    case 'S': return WM_KEY_F1 + 3;
    default:  return WM_KEY_UNKNOWN;
    }
}

/* "ESC [ n ~" number -> keycode, or WM_KEY_UNKNOWN. */
static inline int wm_key_from_tilde(int n)
{
    switch (n) {
    case 2:  return WM_KEY_INSERT;
    case 3:  return WM_KEY_DELETE;
    case 5:  return WM_KEY_PGUP;
    case 6:  return WM_KEY_PGDN;
    case 15: return WM_KEY_F1 + 4;      /* F5 */
    case 17: return WM_KEY_F1 + 5;
    case 18: return WM_KEY_F1 + 6;
    case 19: return WM_KEY_F1 + 7;
    case 20: return WM_KEY_F1 + 8;
    case 21: return WM_KEY_F1 + 9;      /* F10 */
    case 23: return WM_KEY_F1 + 10;
    case 24: return WM_KEY_F1 + 11;     /* F12 */
    default: return WM_KEY_UNKNOWN;
    }
}

/* One plain (non-ESC) byte -> key. */
static inline void wm_key_from_byte(char b, wm_key_t *k)
{
    unsigned char u = (unsigned char)b;
    k->ascii = b;
    if (u == '\r' || u == '\n')       k->keycode = WM_KEY_ENTER;
    else if (u == '\t')               k->keycode = WM_KEY_TAB;
    else if (u == 0x08 || u == 0x7F)  k->keycode = WM_KEY_BACKSPACE;
    else if (u >= 0x01 && u <= 0x1A) {           /* Ctrl-A .. Ctrl-Z */
        k->keycode = 'a' + (u - 1);
        k->mods   |= WM_MOD_CTRL;
    } else if (u >= 0x1C && u <= 0x1F) {         /* Ctrl-\ ] ^ _     */
        static const char ctl[4] = { '\\', ']', '^', '_' };
        k->keycode = ctl[u - 0x1C];
        k->mods   |= WM_MOD_CTRL;
    } else if (u >= 'A' && u <= 'Z') {
        k->keycode = u - 'A' + 'a';
        k->mods   |= WM_MOD_SHIFT;
    } else {
        k->keycode = u;                          /* printable, or 0 */
    }
}

/* Parse "ESC [ ..." at buf (buf[0] = ESC, buf[1] = '[').  Returns bytes
 * consumed, or 0 if no final byte arrives within n. */
static inline int wm_key_csi(const char *buf, int n, wm_key_t *k)
{
    int p[2] = { 0, 0 }, np = 0, have = 0;
    for (int i = 2; i < n && i < WM_KEY_SEQ_MAX; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            if (np < 2) p[np] = p[np] * 10 + (c - '0');
            have = 1;
        } else if (c == ';') {
            if (np < 2) np++;
            have = 0;
        } else if ((unsigned char)c >= 0x40 && (unsigned char)c <= 0x7E) {
            int nparams = np + (have ? 1 : 0);
            if (c == '~') {
                k->keycode = wm_key_from_tilde(p[0]);
                if (nparams >= 2) k->mods |= wm_key_mods_from_param(p[1]);
            } else {
                k->keycode = wm_key_from_final(c);
                if (nparams >= 2) k->mods |= wm_key_mods_from_param(p[1]);
            }
            k->ascii = 0;
            return i + 1;
        } else {
            return 0;                    /* not a CSI byte: malformed */
        }
    }
    return 0;
}

/*
 * wm_key_decode -- decode one key from the front of buf[0..n).
 * Returns the number of bytes consumed (>= 1 when n >= 1; 0 when n <= 0).
 */
static inline int wm_key_decode(const char *buf, int n, wm_key_t *k)
{
    k->keycode = WM_KEY_NONE;
    k->mods    = 0;
    k->ascii   = 0;
    k->seq_len = 0;
    if (n <= 0) return 0;

    int used = 1;
    if (buf[0] != 0x1B) {
        wm_key_from_byte(buf[0], k);
    } else if (n == 1) {
        k->keycode = WM_KEY_ESC;              /* a lone ESC: the Esc key */
        k->ascii   = 0x1B;
    } else if (buf[1] == '[') {
        used = wm_key_csi(buf, n, k);
        if (used == 0) {                      /* malformed: Esc, then rest */
            k->keycode = WM_KEY_ESC; k->ascii = 0x1B; used = 1;
        }
    } else if (buf[1] == 'O' && n >= 3) {     /* SS3: F1-F4 (and arrows)  */
        k->keycode = wm_key_from_final(buf[2]);
        used = 3;
    } else if (buf[1] == 0x1B) {
        /* ESC ESC ...: Alt + whatever the second ESC starts. */
        wm_key_t inner;
        int m = wm_key_decode(buf + 1, n - 1, &inner);
        if (m + 1 > WM_KEY_SEQ_MAX) m = WM_KEY_SEQ_MAX - 1;
        *k = inner;
        k->mods |= WM_MOD_ALT;
        used = m + 1;
    } else {
        /* ESC <char>: Alt + that character ("meta sends escape"). */
        wm_key_from_byte(buf[1], k);
        k->mods |= WM_MOD_ALT;
        used = 2;
    }

    if (used > WM_KEY_SEQ_MAX) used = WM_KEY_SEQ_MAX;
    for (int i = 0; i < used; i++) k->seq[i] = buf[i];
    k->seq_len = used;
    return used;
}

#endif /* _USER_WM_WM_KEYS_H */
