/*
 * Quilon OS — ANSI/VT100 CSI escape sequence parser  (section 11.1)
 *
 * Pure C — no x86 asm, no hardware calls, no kernel dependencies.
 * Compilable and testable on the host (tests/test_ansi.c).
 *
 * Supports the VT100/ECMA-48 CSI sequences used by real-world CLI tools:
 *
 *   ESC [ Pn ; Pn H   — cursor position        (H, f)
 *   ESC [ Pn A/B/C/D  — cursor up/down/right/left
 *   ESC [ 2 J         — erase display
 *   ESC [ Pn K        — erase in line (0=to EOL, 1=to SOL, 2=whole)
 *   ESC [ Pn m        — SGR: bold, foreground/background colors
 *   ESC [ s           — save cursor position
 *   ESC [ u           — restore cursor position
 *
 * Usage:
 *   ansi_parser_t p;
 *   ansi_parser_init(&p);
 *
 *   ansi_event_t ev;
 *   switch (ansi_feed(&p, c, &ev)) {
 *   case ANSI_CHAR: render(ev.ch);         break;
 *   case ANSI_CSI:  execute_csi(&ev);      break;
 *   case ANSI_NONE: break;  // partial sequence, keep feeding
 *   }
 */

#ifndef _KERNEL_ANSI_H
#define _KERNEL_ANSI_H

#include <stdint.h>

/* ── Return values from ansi_feed ─────────────────────────────────────── */
#define ANSI_NONE  0   /* character consumed; sequence not yet complete */
#define ANSI_CHAR  1   /* plain character to render: ev->ch             */
#define ANSI_CSI   2   /* complete CSI sequence: ev->cmd, params        */

/* ── Parser limits ────────────────────────────────────────────────────── */
#define ANSI_MAX_PARAMS  8   /* max semicolon-separated integers per CSI */
#define ANSI_BUF_SIZE   24   /* max bytes accumulated between ESC[ and cmd */

/* ── Parser states ────────────────────────────────────────────────────── */
typedef enum {
    ANSI_STATE_NORMAL  = 0,
    ANSI_STATE_ESC     = 1,   /* saw 0x1B */
    ANSI_STATE_CSI     = 2,   /* saw 0x1B 0x5B ('[') */
} ansi_state_t;

/* ── Parser context ───────────────────────────────────────────────────── */
typedef struct {
    ansi_state_t state;
    char         buf[ANSI_BUF_SIZE];  /* parameter characters */
    int          len;
} ansi_parser_t;

/* ── Parsed event ─────────────────────────────────────────────────────── */
typedef struct {
    char ch;                        /* ANSI_CHAR: the character          */
    char cmd;                       /* ANSI_CSI:  final byte (H,A,J,m…) */
    int  params[ANSI_MAX_PARAMS];   /* ANSI_CSI:  integer parameters     */
    int  nparams;                   /* number of valid params[] entries  */
} ansi_event_t;

/* ── ansi_parser_init ─────────────────────────────────────────────────── */

static inline void ansi_parser_init(ansi_parser_t *p)
{
    p->state = ANSI_STATE_NORMAL;
    p->len   = 0;
}

/* ── ansi_parse_params ────────────────────────────────────────────────── */
/*
 * Parse a CSI parameter string (the bytes between '[' and the command
 * byte) into an array of non-negative integers.
 *
 * "31"     → {31},    nparams = 1
 * "1;32"   → {1, 32}, nparams = 2
 * ";"      → {0},     nparams = 1
 * ""       → {},      nparams = 0
 *
 * Parameters separated by ';'; an empty field defaults to 0.
 * Returns the number of parameters stored (≤ max_params).
 */
static inline int ansi_parse_params(const char *buf, int len,
                                    int params[], int max_params)
{
    int n = 0, cur = 0, has_digit = 0;

    if (len == 0 || max_params == 0)
        return 0;

    for (int i = 0; i < len && n < max_params; i++) {
        char c = buf[i];
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            has_digit = 1;
        } else if (c == ';') {
            params[n++] = has_digit ? cur : 0;
            cur       = 0;
            has_digit = 0;
        }
        /* ignore other bytes (e.g. '?' for private sequences) */
    }
    /* emit last field only if there was a digit after the final ';' */
    if (n < max_params && has_digit)
        params[n++] = cur;

    return n;
}

/* ── ansi_sgr_color ───────────────────────────────────────────────────── */
/*
 * Map an ANSI SGR color code to a 0x00RRGGBB pixel value.
 *
 * Codes 30-37  → standard foreground colors
 * Codes 40-47  → standard background colors (same palette)
 * Codes 90-97  → bright foreground colors
 * Codes 100-107 → bright background colors
 *
 * Returns 0xFFFFFFFF for unrecognised codes.
 */
static inline uint32_t ansi_sgr_color(int code)
{
    static const uint32_t std8[8] = {
        0x000000u,   /* 0: black   */
        0xAA0000u,   /* 1: red     */
        0x00AA00u,   /* 2: green   */
        0xAAAA00u,   /* 3: yellow  */
        0x0000AAu,   /* 4: blue    */
        0xAA00AAu,   /* 5: magenta */
        0x00AAAAu,   /* 6: cyan    */
        0xAAAAAAu,   /* 7: white   */
    };
    static const uint32_t bright8[8] = {
        0x555555u,   /* 0: bright black (dark grey) */
        0xFF5555u,   /* 1: bright red               */
        0x55FF55u,   /* 2: bright green             */
        0xFFFF55u,   /* 3: bright yellow            */
        0x5555FFu,   /* 4: bright blue              */
        0xFF55FFu,   /* 5: bright magenta           */
        0x55FFFFu,   /* 6: bright cyan              */
        0xFFFFFFu,   /* 7: bright white             */
    };

    if (code >= 30 && code <= 37)  return std8[code - 30];
    if (code >= 40 && code <= 47)  return std8[code - 40];
    if (code >= 90 && code <= 97)  return bright8[code - 90];
    if (code >= 100 && code <= 107) return bright8[code - 100];
    return 0xFFFFFFFFu;
}

/* ── ansi_feed ────────────────────────────────────────────────────────── */
/*
 * Feed one character to the parser.
 *
 * Returns:
 *   ANSI_NONE — character was consumed as part of a partial escape sequence.
 *   ANSI_CHAR — ev->ch holds the plain character to render.
 *   ANSI_CSI  — ev->cmd, ev->params[0..nparams-1] describe a complete CSI command.
 */
static inline int ansi_feed(ansi_parser_t *p, char c, ansi_event_t *ev)
{
    switch (p->state) {

    case ANSI_STATE_NORMAL:
        if (c == '\033') {         /* ESC */
            p->state = ANSI_STATE_ESC;
            return ANSI_NONE;
        }
        ev->ch = c;
        return ANSI_CHAR;

    case ANSI_STATE_ESC:
        if (c == '[') {            /* CSI introducer */
            p->state = ANSI_STATE_CSI;
            p->len   = 0;
            return ANSI_NONE;
        }
        /* Unrecognised ESC sequence: reset, emit the char literally */
        p->state = ANSI_STATE_NORMAL;
        ev->ch = c;
        return ANSI_CHAR;

    case ANSI_STATE_CSI:
        /* Final byte: 0x40–0x7E (letters + a few punctuation chars) */
        if (c >= 0x40 && c <= 0x7E) {
            ev->cmd    = c;
            ev->nparams = ansi_parse_params(p->buf, p->len,
                                             ev->params, ANSI_MAX_PARAMS);
            p->state   = ANSI_STATE_NORMAL;
            return ANSI_CSI;
        }
        /* Parameter / intermediate byte — accumulate */
        if (p->len < ANSI_BUF_SIZE - 1)
            p->buf[p->len++] = c;
        return ANSI_NONE;
    }

    return ANSI_NONE; /* unreachable */
}

#endif /* _KERNEL_ANSI_H */
