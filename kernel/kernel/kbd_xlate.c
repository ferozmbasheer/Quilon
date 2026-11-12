/*
 * Quilon OS -- PS/2 scancode set 1 translation (ROADMAP3 17.0.1)
 *
 * See <kernel/kbd_xlate.h> for the output format.  Pure C so tests/test_kbd.c
 * can drive it on the host.
 */

#include <kernel/kbd_xlate.h>

/* Scancode set 1, make codes 0x00-0x58: unshifted and shifted US layout.
 * 0 = no character (modifier, lock, function or navigation key -- those are
 * handled separately below). */
static const char map_plain[0x59] = {
    /* 00 */ 0,    27,  '1', '2', '3', '4', '5', '6',
    /* 08 */ '7', '8', '9', '0', '-', '=', '\b', '\t',
    /* 10 */ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
    /* 18 */ 'o', 'p', '[', ']', '\n', 0,   'a', 's',
    /* 20 */ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    /* 28 */ '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    /* 30 */ 'b', 'n', 'm', ',', '.', '/', 0,   '*',
    /* 38 */ 0,   ' ', 0,   0,   0,   0,   0,   0,
    /* 40 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 48 */ 0,   0,   '-', 0,   0,   0,   '+', 0,
    /* 50 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 58 */ 0,
};

static const char map_shift[0x59] = {
    /* 00 */ 0,    27,  '!', '@', '#', '$', '%', '^',
    /* 08 */ '&', '*', '(', ')', '_', '+', '\b', '\t',
    /* 10 */ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
    /* 18 */ 'O', 'P', '{', '}', '\n', 0,   'A', 'S',
    /* 20 */ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
    /* 28 */ '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    /* 30 */ 'B', 'N', 'M', '<', '>', '?', 0,   '*',
    /* 38 */ 0,   ' ', 0,   0,   0,   0,   0,   0,
    /* 40 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 48 */ 0,   0,   '-', 0,   0,   0,   '+', 0,
    /* 50 */ 0,   0,   0,   0,   0,   0,   0,   0,
    /* 58 */ 0,
};

/* Modifier / lock make codes. */
#define SC_LCTRL   0x1D   /* E0 1D = right Ctrl */
#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_LALT    0x38   /* E0 38 = right Alt (AltGr) */
#define SC_CAPS    0x3A

/* Escape sequence for a navigation/function key, or NULL.  The navigation
 * keys exist both as E0-prefixed dedicated keys and (unprefixed) on the
 * keypad with Num Lock off; Quilon has no Num Lock state, so the keypad
 * always acts as navigation, as it did before (it produced nothing then). */
static const char *special_seq(uint8_t sc)
{
    switch (sc) {
    case 0x48: return "\x1b[A";    /* Up    */
    case 0x50: return "\x1b[B";    /* Down  */
    case 0x4D: return "\x1b[C";    /* Right */
    case 0x4B: return "\x1b[D";    /* Left  */
    case 0x47: return "\x1b[H";    /* Home  */
    case 0x4F: return "\x1b[F";    /* End   */
    case 0x52: return "\x1b[2~";   /* Insert */
    case 0x53: return "\x1b[3~";   /* Delete */
    case 0x49: return "\x1b[5~";   /* PgUp  */
    case 0x51: return "\x1b[6~";   /* PgDn  */
    case 0x3B: return "\x1bOP";    /* F1 */
    case 0x3C: return "\x1bOQ";    /* F2 */
    case 0x3D: return "\x1bOR";    /* F3 */
    case 0x3E: return "\x1bOS";    /* F4 */
    case 0x3F: return "\x1b[15~";  /* F5 */
    case 0x40: return "\x1b[17~";  /* F6 */
    case 0x41: return "\x1b[18~";  /* F7 */
    case 0x42: return "\x1b[19~";  /* F8 */
    case 0x43: return "\x1b[20~";  /* F9 */
    case 0x44: return "\x1b[21~";  /* F10 */
    case 0x57: return "\x1b[23~";  /* F11 */
    case 0x58: return "\x1b[24~";  /* F12 */
    default:   return 0;
    }
}

static int emit_str(const char *s, char *out)
{
    int n = 0;
    while (s[n] && n < KBD_SEQ_MAX) { out[n] = s[n]; n++; }
    return n;
}

/*
 * A navigation/function key pressed with Shift, Alt or Ctrl: rewrite its
 * plain sequence into the xterm parameter form (ROADMAP3 18.2), so the
 * modifier is not lost -- Alt-F4 used to arrive as a bare F4.
 *   ESC [ X   / ESC O X  ->  ESC [ 1 ; m X
 *   ESC [ n ~            ->  ESC [ n ; m ~
 * m = 1 + Shift(1) + Alt(2) + Ctrl(4).  Longest: ESC[24;8~ = 7 bytes.
 */
static int emit_modified(const char *seq, int m, char *out)
{
    int n = 0, len = 0;
    while (seq[len]) len++;
    out[n++] = 0x1B;
    out[n++] = '[';
    if (seq[len - 1] == '~') {
        for (int i = 2; i < len - 1; i++) out[n++] = seq[i];   /* n */
    } else {
        out[n++] = '1';
    }
    out[n++] = ';';
    out[n++] = (char)('0' + m);
    out[n++] = seq[len - 1];                                   /* final */
    return n;
}

void kbd_state_init(kbd_state_t *st)
{
    st->shift_l = st->shift_r = 0;
    st->ctrl_l  = st->ctrl_r  = 0;
    st->alt_l   = st->alt_r   = 0;
    st->caps    = 0;
    st->e0      = 0;
    st->e1_skip = 0;
}

int kbd_translate(kbd_state_t *st, uint8_t scancode, char *out)
{
    /* Pause/Break sends E1 1D 45 E1 9D C5 and has no meaning here.  Swallow
     * it whole: its 1D byte would otherwise look like a Ctrl press. */
    if (st->e1_skip) { st->e1_skip--; return 0; }
    if (scancode == 0xE1) { st->e1_skip = 5; return 0; }
    if (scancode == 0xE0) { st->e0 = 1; return 0; }

    int     ext     = st->e0;
    int     release = (scancode & 0x80) != 0;
    uint8_t sc      = scancode & 0x7F;
    st->e0 = 0;

    /* Modifiers: track press and release. */
    uint8_t down = release ? 0 : 1;
    switch (sc) {
    case SC_LSHIFT:
    case SC_RSHIFT:
        /* E0 2A / E0 36 are "fake shifts" around Print Screen etc.  */
        if (ext) return 0;
        if (sc == SC_LSHIFT) st->shift_l = down; else st->shift_r = down;
        return 0;
    case SC_LCTRL:
        if (ext) st->ctrl_r = down; else st->ctrl_l = down;
        return 0;
    case SC_LALT:
        if (ext) st->alt_r = down; else st->alt_l = down;
        return 0;
    case SC_CAPS:
        if (!release && !ext) st->caps ^= 1;
        return 0;
    default:
        break;
    }

    if (release) return 0;

    int alt = st->alt_l || st->alt_r;
    int n   = 0;

    /* Navigation / function keys. */
    const char *seq = special_seq(sc);
    if (seq) {
        /* Unprefixed 0x4A '-' and 0x4E '+' fall through below; keypad 0x37
         * '*' is not in special_seq either. */
        int m = 1 + ((st->shift_l || st->shift_r) ? 1 : 0) + (alt ? 2 : 0) +
                ((st->ctrl_l || st->ctrl_r) ? 4 : 0);
        if (m > 1) return emit_modified(seq, m, out);
        return emit_str(seq, out);
    }

    if (ext) {
        /* Remaining E0 keys: keypad Enter and keypad '/'.  Everything else
         * (Print Screen, Windows keys, media keys) produces nothing. */
        if (sc == 0x1C) { out[0] = '\n'; return 1; }
        if (sc == 0x35) { out[0] = '/';  return 1; }
        return 0;
    }

    if (sc >= sizeof map_plain) return 0;

    int  shift = st->shift_l || st->shift_r;
    char c     = shift ? map_shift[sc] : map_plain[sc];
    if (c == 0) return 0;

    /* Caps Lock inverts the case of letters only. */
    if (st->caps) {
        if (c >= 'a' && c <= 'z')      c = (char)(c - 'a' + 'A');
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }

    if (st->ctrl_l || st->ctrl_r) {
        char base = map_plain[sc];
        if (base >= 'a' && base <= 'z')
            c = (char)(base - 'a' + 1);          /* Ctrl-A .. Ctrl-Z */
        else if (base == '[')  c = 0x1B;
        else if (base == '\\') c = 0x1C;
        else if (base == ']')  c = 0x1D;
        /* any other key: unmodified character */
    }

    if (alt) out[n++] = 0x1B;
    out[n++] = c;
    return n;
}
