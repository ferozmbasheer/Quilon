/*
 * Quilon OS -- Graphical Terminal emulator logic (section 14.3)
 *
 * Pure-logic header: ANSI escape-code parser + 80×24 cell grid.
 * No syscalls, no libgfx calls — fully testable on the host.
 *
 * The emulator state (gterm_t) owns a 2-D grid of cells.  Each cell
 * records a character, foreground colour, and background colour.
 * gterm_write() feeds raw bytes (e.g. from a shell's stdout) through
 * the ANSI state machine, updating the grid and cursor position.
 * The renderer (in main.c) reads the grid and calls gfx_draw_text().
 *
 * ANSI subset implemented (covers ~90% of shell output):
 *   \n  \r  \b  — newline / carriage return / backspace
 *   \033[H      — cursor home (1,1)
 *   \033[r;cH   — cursor move to row r, col c (1-based)
 *   \033[A/B/C/D — cursor up / down / right / left
 *   \033[2J     — clear screen
 *   \033[K      — erase to end of line
 *   \033[0m     — reset SGR attributes
 *   \033[1m     — bold (renders as bright foreground)
 *   \033[3Xm    — foreground colour (30–37, 90–97)
 *   \033[4Xm    — background colour (40–47)
 *   Multiple params separated by ';' in a single CSI sequence.
 */

#ifndef _GTERM_H
#define _GTERM_H

#include <stdint.h>

/* ── Dimensions ─────────────────────────────────────────────────────────── */

#define GTERM_COLS       80
#define GTERM_ROWS       24
#define GTERM_CELL_W      8   /* pixels per character cell (GFX_CHAR_W) */
#define GTERM_CELL_H     16   /* pixels per character cell (GFX_CHAR_H) */

/* Pixel size of the terminal content area */
#define GTERM_PX_W  (GTERM_COLS * GTERM_CELL_W)
#define GTERM_PX_H  (GTERM_ROWS * GTERM_CELL_H)

/* ── Colour type ─────────────────────────────────────────────────────────── */

typedef uint32_t gterm_color_t;   /* 0x00RRGGBB — same layout as libgfx */

#define GT_RGB(r,g,b) (((gterm_color_t)(r)<<16)|((gterm_color_t)(g)<<8)|(b))

/* Standard ANSI 8-colour palette (normal + bright) */
static const gterm_color_t gterm_palette[16] = {
    GT_RGB(  0,   0,   0),   /* 0 black   */
    GT_RGB(170,   0,   0),   /* 1 red     */
    GT_RGB(  0, 170,   0),   /* 2 green   */
    GT_RGB(170, 170,   0),   /* 3 yellow  */
    GT_RGB(  0,   0, 170),   /* 4 blue    */
    GT_RGB(170,   0, 170),   /* 5 magenta */
    GT_RGB(  0, 170, 170),   /* 6 cyan    */
    GT_RGB(170, 170, 170),   /* 7 white   */
    /* bright variants (90–97 / 100–107) */
    GT_RGB( 85,  85,  85),   /* 8  bright black  */
    GT_RGB(255,  85,  85),   /* 9  bright red    */
    GT_RGB( 85, 255,  85),   /* 10 bright green  */
    GT_RGB(255, 255,  85),   /* 11 bright yellow */
    GT_RGB( 85,  85, 255),   /* 12 bright blue   */
    GT_RGB(255,  85, 255),   /* 13 bright magenta*/
    GT_RGB( 85, 255, 255),   /* 14 bright cyan   */
    GT_RGB(255, 255, 255),   /* 15 bright white  */
};

#define GT_DEFAULT_FG  gterm_palette[7]   /* light grey */
#define GT_DEFAULT_BG  GT_RGB(0, 0, 0)    /* black      */

/* ── Cell ────────────────────────────────────────────────────────────────── */

typedef struct {
    char          ch;
    gterm_color_t fg;
    gterm_color_t bg;
} gterm_cell_t;

/* ── ANSI parser state ───────────────────────────────────────────────────── */

typedef enum {
    GTERM_ST_NORMAL = 0,
    GTERM_ST_ESC,      /* saw \033 */
    GTERM_ST_CSI,      /* saw \033[ */
} gterm_ansi_state_t;

#define GTERM_ANSI_BUF  32   /* max param bytes before the final byte */
#define GTERM_MAX_PARAMS 8   /* max semicolon-separated integers */

/* ── Terminal state ──────────────────────────────────────────────────────── */

typedef struct {
    gterm_cell_t       cells[GTERM_ROWS][GTERM_COLS];
    int                cursor_row;
    int                cursor_col;
    gterm_color_t      cur_fg;
    gterm_color_t      cur_bg;
    int                bold;          /* 1 if SGR 1 (bold) is active */

    /* ANSI parser */
    gterm_ansi_state_t ansi_state;
    char               ansi_buf[GTERM_ANSI_BUF];
    int                ansi_len;
} gterm_t;

/* ── Internal helpers (static inline — no external dependencies) ─────────── */

/* Parse up to GTERM_MAX_PARAMS semicolon-separated integers from str.
 * Unspecified params default to 0. Returns number of params found.          */
static inline int gterm_parse_params(const char *str, int params[],
                                     int max_params)
{
    int n = 0;
    int val = 0;
    int any = 0;
    const char *p;
    for (p = str; *p; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            val = val * 10 + (c - '0');
            any = 1;
        } else if (c == ';') {
            if (n < max_params) params[n++] = any ? val : 0;
            val = 0; any = 0;
        }
    }
    if (n < max_params) params[n++] = any ? val : 0;
    return n;
}

/* Scroll the grid up by one row; fill the new bottom row with blanks.       */
static inline void gterm_scroll_up(gterm_t *t)
{
    int r, c;
    for (r = 0; r < GTERM_ROWS - 1; r++)
        for (c = 0; c < GTERM_COLS; c++)
            t->cells[r][c] = t->cells[r + 1][c];

    for (c = 0; c < GTERM_COLS; c++) {
        t->cells[GTERM_ROWS - 1][c].ch = ' ';
        t->cells[GTERM_ROWS - 1][c].fg = t->cur_fg;
        t->cells[GTERM_ROWS - 1][c].bg = t->cur_bg;
    }
}

/* Advance cursor to the next line, scrolling if at the bottom.              */
static inline void gterm_newline(gterm_t *t)
{
    t->cursor_col = 0;
    t->cursor_row++;
    if (t->cursor_row >= GTERM_ROWS) {
        gterm_scroll_up(t);
        t->cursor_row = GTERM_ROWS - 1;
    }
}

/* Write a printable character at the cursor and advance.                    */
static inline void gterm_put_char(gterm_t *t, char ch)
{
    if (t->cursor_col >= GTERM_COLS) {
        t->cursor_col = 0;
        t->cursor_row++;
        if (t->cursor_row >= GTERM_ROWS) {
            gterm_scroll_up(t);
            t->cursor_row = GTERM_ROWS - 1;
        }
    }
    t->cells[t->cursor_row][t->cursor_col].ch = ch;
    t->cells[t->cursor_row][t->cursor_col].fg = t->cur_fg;
    t->cells[t->cursor_row][t->cursor_col].bg = t->cur_bg;
    t->cursor_col++;
}

/* Clamp helper */
static inline int gterm_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Dispatch a completed CSI sequence.
 * params_str is the parameter string; cmd is the final byte.                */
static inline void gterm_dispatch_csi(gterm_t *t,
                                      const char *params_str, char cmd)
{
    int params[GTERM_MAX_PARAMS];
    int i;
    for (i = 0; i < GTERM_MAX_PARAMS; i++) params[i] = 0;
    int np = gterm_parse_params(params_str, params, GTERM_MAX_PARAMS);

    switch (cmd) {
    case 'H': {   /* \033[r;cH -- cursor position (1-based, default 1) */
        int r = params[0] ? params[0] - 1 : 0;
        int c = (np >= 2 && params[1]) ? params[1] - 1 : 0;
        t->cursor_row = gterm_clamp(r, 0, GTERM_ROWS - 1);
        t->cursor_col = gterm_clamp(c, 0, GTERM_COLS - 1);
        break;
    }
    case 'A': {   /* cursor up */
        int n = params[0] ? params[0] : 1;
        t->cursor_row = gterm_clamp(t->cursor_row - n, 0, GTERM_ROWS - 1);
        break;
    }
    case 'B': {   /* cursor down */
        int n = params[0] ? params[0] : 1;
        t->cursor_row = gterm_clamp(t->cursor_row + n, 0, GTERM_ROWS - 1);
        break;
    }
    case 'C': {   /* cursor right */
        int n = params[0] ? params[0] : 1;
        t->cursor_col = gterm_clamp(t->cursor_col + n, 0, GTERM_COLS - 1);
        break;
    }
    case 'D': {   /* cursor left */
        int n = params[0] ? params[0] : 1;
        t->cursor_col = gterm_clamp(t->cursor_col - n, 0, GTERM_COLS - 1);
        break;
    }
    case 'J': {   /* \033[2J -- clear screen */
        if (params[0] == 2) {
            int r, c;
            for (r = 0; r < GTERM_ROWS; r++)
                for (c = 0; c < GTERM_COLS; c++) {
                    t->cells[r][c].ch = ' ';
                    t->cells[r][c].fg = t->cur_fg;
                    t->cells[r][c].bg = t->cur_bg;
                }
            t->cursor_row = 0;
            t->cursor_col = 0;
        }
        break;
    }
    case 'K': {   /* \033[K -- erase to end of line */
        int c;
        for (c = t->cursor_col; c < GTERM_COLS; c++) {
            t->cells[t->cursor_row][c].ch = ' ';
            t->cells[t->cursor_row][c].fg = t->cur_fg;
            t->cells[t->cursor_row][c].bg = t->cur_bg;
        }
        break;
    }
    case 'm': {   /* SGR -- select graphic rendition */
        int pi;
        for (pi = 0; pi < np; pi++) {
            int p = params[pi];
            if (p == 0) {
                /* Reset all attributes */
                t->cur_fg = GT_DEFAULT_FG;
                t->cur_bg = GT_DEFAULT_BG;
                t->bold   = 0;
            } else if (p == 1) {
                t->bold = 1;
            } else if (p == 22) {
                t->bold = 0;
            } else if (p >= 30 && p <= 37) {
                int idx = p - 30 + (t->bold ? 8 : 0);
                t->cur_fg = gterm_palette[idx];
            } else if (p >= 40 && p <= 47) {
                t->cur_bg = gterm_palette[p - 40];
            } else if (p >= 90 && p <= 97) {
                t->cur_fg = gterm_palette[p - 90 + 8];
            } else if (p >= 100 && p <= 107) {
                t->cur_bg = gterm_palette[p - 100 + 8];
            } else if (p == 39) {
                t->cur_fg = GT_DEFAULT_FG;
            } else if (p == 49) {
                t->cur_bg = GT_DEFAULT_BG;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Initialise terminal state: blank cells, cursor at (0,0), default colours. */
static inline void gterm_init(gterm_t *t)
{
    int r, c;
    t->cursor_row  = 0;
    t->cursor_col  = 0;
    t->cur_fg      = GT_DEFAULT_FG;
    t->cur_bg      = GT_DEFAULT_BG;
    t->bold        = 0;
    t->ansi_state  = GTERM_ST_NORMAL;
    t->ansi_len    = 0;
    t->ansi_buf[0] = '\0';
    for (r = 0; r < GTERM_ROWS; r++)
        for (c = 0; c < GTERM_COLS; c++) {
            t->cells[r][c].ch = ' ';
            t->cells[r][c].fg = GT_DEFAULT_FG;
            t->cells[r][c].bg = GT_DEFAULT_BG;
        }
}

/* Feed one byte through the ANSI state machine, updating the cell grid.     */
static inline void gterm_write_char(gterm_t *t, char ch)
{
    switch (t->ansi_state) {
    case GTERM_ST_NORMAL:
        if (ch == '\033') {
            t->ansi_state = GTERM_ST_ESC;
        } else if (ch == '\n') {
            gterm_newline(t);
        } else if (ch == '\r') {
            t->cursor_col = 0;
        } else if (ch == '\b' || ch == 127) {
            if (t->cursor_col > 0) t->cursor_col--;
        } else if ((unsigned char)ch >= 32) {
            gterm_put_char(t, ch);
        }
        break;

    case GTERM_ST_ESC:
        if (ch == '[') {
            t->ansi_state = GTERM_ST_CSI;
            t->ansi_len   = 0;
            t->ansi_buf[0] = '\0';
        } else {
            t->ansi_state = GTERM_ST_NORMAL;
        }
        break;

    case GTERM_ST_CSI:
        if (ch >= 0x40 && ch <= 0x7E) {
            /* Final byte: dispatch and return to normal. */
            t->ansi_buf[t->ansi_len] = '\0';
            gterm_dispatch_csi(t, t->ansi_buf, ch);
            t->ansi_state = GTERM_ST_NORMAL;
        } else if (t->ansi_len < GTERM_ANSI_BUF - 1) {
            t->ansi_buf[t->ansi_len++] = ch;
        }
        break;
    }
}

/* Feed a buffer of bytes into the terminal.                                 */
static inline void gterm_write(gterm_t *t, const char *buf, int len)
{
    int i;
    for (i = 0; i < len; i++)
        gterm_write_char(t, buf[i]);
}

#endif /* _GTERM_H */
