/*
 * test_gterm.c -- host-side unit tests for the Graphical Terminal emulator
 *                 (Quilon OS section 14.3)
 *
 * Compiled on the host with native gcc; no cross-compiler, no QEMU.
 *
 * What is tested
 * ──────────────
 *   Initialisation
 *     - gterm_init resets cursor to (0,0) and fills grid with spaces
 *     - default foreground and background colours are set correctly
 *
 *   Plain text / control characters
 *     - printable characters written to cells at the correct position
 *     - cursor advances after each character
 *     - newline (\n) moves cursor to next row, column 0
 *     - carriage return (\r) moves cursor to column 0 without changing row
 *     - backspace (\b) moves cursor left by one
 *     - backspace at column 0 does not go negative
 *
 *   Wrap and scroll
 *     - writing past column 79 wraps to the next row
 *     - writing past row 23 scrolls the grid up by one
 *     - after scroll, row 0 contains what was in row 1
 *     - last row is blank after a scroll
 *
 *   ANSI escape sequences
 *     - \033[H moves cursor to (0,0)
 *     - \033[r;cH moves cursor to (r-1, c-1)
 *     - \033[A  moves cursor up by 1
 *     - \033[3A moves cursor up by 3
 *     - \033[B  moves cursor down by 1
 *     - \033[C  moves cursor right by 1
 *     - \033[D  moves cursor left by 1
 *     - cursor up/down/left/right clamp at grid boundaries
 *     - \033[2J clears the screen and homes the cursor
 *     - \033[K  erases to end of line
 *     - \033[0m resets fg/bg to defaults
 *     - \033[1m sets bold; fg brightens (30+0→palette[8])
 *     - \033[32m sets fg to green
 *     - \033[41m sets bg to red
 *     - \033[1;32m sets bold AND fg in one sequence
 *     - \033[90m sets bright fg via 90–97 range
 *     - incomplete escape sequence (ESC with no '[') is ignored, not crashed
 *     - overlong CSI parameter string (≥ GTERM_ANSI_BUF) is truncated safely
 *
 *   gterm_write (batch)
 *     - feeding a multi-character string matches char-by-char calls
 *     - mixed plain text + ANSI produces correct cell colours
 *
 *   Param parser
 *     - empty string gives one param with value 0
 *     - single integer is parsed correctly
 *     - two integers separated by ';' are both parsed
 *     - trailing ';' adds a zero param
 */

#include <string.h>
#include <stddef.h>
#include "framework.h"

/* Include gterm.h directly — all logic is static inline. */
#include "../user/gterm/gterm.h"

/* ── helpers ─────────────────────────────────────────────────────────────── */

static gterm_t make_fresh(void)
{
    gterm_t t;
    gterm_init(&t);
    return t;
}

/* Write a C string (without the NUL) into the terminal. */
static void feed(gterm_t *t, const char *s)
{
    gterm_write(t, s, (int)strlen(s));
}

/* ── Initialisation ──────────────────────────────────────────────────────── */

static void test_init(void)
{
    gterm_t t = make_fresh();

    ASSERT_EQ(t.cursor_row, 0,   "cursor row starts at 0");
    ASSERT_EQ(t.cursor_col, 0,   "cursor col starts at 0");
    ASSERT_EQ(t.bold,       0,   "bold starts off");
    ASSERT_EQ((int)t.ansi_state, (int)GTERM_ST_NORMAL, "parser in NORMAL state");

    /* All cells should be blank (space) */
    int blank = 1;
    int r, c;
    for (r = 0; r < GTERM_ROWS; r++)
        for (c = 0; c < GTERM_COLS; c++)
            if (t.cells[r][c].ch != ' ') { blank = 0; break; }
    ASSERT_EQ(blank, 1, "all cells initialised to space");

    /* Default colours */
    ASSERT_EQ((int)t.cur_fg, (int)GT_DEFAULT_FG, "default fg colour");
    ASSERT_EQ((int)t.cur_bg, (int)GT_DEFAULT_BG, "default bg colour");
    ASSERT_EQ((int)t.cells[0][0].fg, (int)GT_DEFAULT_FG, "cell (0,0) fg = default");
    ASSERT_EQ((int)t.cells[0][0].bg, (int)GT_DEFAULT_BG, "cell (0,0) bg = default");
}

/* ── Plain text ──────────────────────────────────────────────────────────── */

static void test_plain_text(void)
{
    gterm_t t = make_fresh();

    gterm_write_char(&t, 'H');
    ASSERT_EQ(t.cells[0][0].ch, 'H', "first char stored at (0,0)");
    ASSERT_EQ(t.cursor_col, 1,        "cursor advanced to col 1");
    ASSERT_EQ(t.cursor_row, 0,        "cursor still on row 0");

    gterm_write_char(&t, 'i');
    ASSERT_EQ(t.cells[0][1].ch, 'i', "second char stored at (0,1)");
    ASSERT_EQ(t.cursor_col, 2,        "cursor advanced to col 2");

    /* Characters carry the current fg/bg */
    ASSERT_EQ((int)t.cells[0][0].fg, (int)GT_DEFAULT_FG, "char fg = current fg");
    ASSERT_EQ((int)t.cells[0][0].bg, (int)GT_DEFAULT_BG, "char bg = current bg");
}

static void test_control_newline(void)
{
    gterm_t t = make_fresh();
    feed(&t, "AB\nC");

    ASSERT_EQ(t.cells[0][0].ch, 'A', "A at (0,0)");
    ASSERT_EQ(t.cells[0][1].ch, 'B', "B at (0,1)");
    ASSERT_EQ(t.cells[1][0].ch, 'C', "C at (1,0) after newline");
    ASSERT_EQ(t.cursor_row, 1,        "cursor on row 1 after newline");
    ASSERT_EQ(t.cursor_col, 1,        "cursor at col 1 after C");
}

static void test_control_cr(void)
{
    gterm_t t = make_fresh();
    feed(&t, "ABC\rD");

    ASSERT_EQ(t.cells[0][0].ch, 'D', "D overwrote A after CR");
    ASSERT_EQ(t.cursor_row, 0,        "still on row 0 after CR");
    ASSERT_EQ(t.cursor_col, 1,        "cursor at col 1 after D");
}

static void test_control_backspace(void)
{
    gterm_t t = make_fresh();
    feed(&t, "AB\bC");

    ASSERT_EQ(t.cursor_col, 2,        "col=2 after A,B,BS,C");
    ASSERT_EQ(t.cells[0][1].ch, 'C', "C overwrote B after backspace");

    /* Backspace at col 0 should clamp */
    gterm_t t2 = make_fresh();
    gterm_write_char(&t2, '\b');
    ASSERT_EQ(t2.cursor_col, 0, "backspace at col 0 does not go negative");
}

/* ── Wrap and scroll ─────────────────────────────────────────────────────── */

static void test_wrap(void)
{
    gterm_t t = make_fresh();
    int c;

    /* Fill row 0 completely */
    for (c = 0; c < GTERM_COLS; c++)
        gterm_write_char(&t, 'X');

    /* Writing one more character should wrap to (1,0). */
    gterm_write_char(&t, 'Y');
    ASSERT_EQ(t.cursor_row, 1,        "wrapped to row 1");
    ASSERT_EQ(t.cursor_col, 1,        "col=1 after Y on row 1");
    ASSERT_EQ(t.cells[1][0].ch, 'Y', "Y at (1,0)");
}

static void test_scroll(void)
{
    gterm_t t = make_fresh();
    int r;

    /* Write one character per row to fill all rows. */
    for (r = 0; r < GTERM_ROWS; r++) {
        gterm_write_char(&t, (char)('A' + (r % 26)));
        gterm_write_char(&t, '\n');
    }

    /* After writing GTERM_ROWS newlines, we must have scrolled.
     * Row 0 should now hold what was originally in row 1 ('B').    */
    ASSERT_EQ(t.cells[0][0].ch, 'B', "row 0 = old row 1 after scroll");

    /* The last row should be blank (scrolled in). */
    int last_blank = 1;
    int c;
    for (c = 0; c < GTERM_COLS; c++)
        if (t.cells[GTERM_ROWS-1][c].ch != ' ') { last_blank = 0; break; }
    ASSERT_EQ(last_blank, 1, "last row is blank after scroll");

    /* Cursor should be on the last row. */
    ASSERT_EQ(t.cursor_row, GTERM_ROWS - 1, "cursor stays at last row after scroll");
}

/* ── ANSI cursor movement ─────────────────────────────────────────────────── */

static void test_ansi_cursor_home(void)
{
    gterm_t t = make_fresh();
    t.cursor_row = 5; t.cursor_col = 10;
    feed(&t, "\033[H");
    ASSERT_EQ(t.cursor_row, 0, "\\033[H homes row");
    ASSERT_EQ(t.cursor_col, 0, "\\033[H homes col");
}

static void test_ansi_cursor_position(void)
{
    gterm_t t = make_fresh();
    feed(&t, "\033[5;12H");
    ASSERT_EQ(t.cursor_row, 4,  "\\033[5;12H → row 4 (0-based)");
    ASSERT_EQ(t.cursor_col, 11, "\\033[5;12H → col 11 (0-based)");
}

static void test_ansi_cursor_up(void)
{
    gterm_t t = make_fresh();
    t.cursor_row = 10;
    feed(&t, "\033[A");
    ASSERT_EQ(t.cursor_row, 9, "\\033[A moves up 1");

    t.cursor_row = 5;
    feed(&t, "\033[3A");
    ASSERT_EQ(t.cursor_row, 2, "\\033[3A moves up 3");

    /* Clamp at row 0 */
    t.cursor_row = 1;
    feed(&t, "\033[5A");
    ASSERT_EQ(t.cursor_row, 0, "cursor up clamps at row 0");
}

static void test_ansi_cursor_down(void)
{
    gterm_t t = make_fresh();
    t.cursor_row = 10;
    feed(&t, "\033[B");
    ASSERT_EQ(t.cursor_row, 11, "\\033[B moves down 1");

    t.cursor_row = GTERM_ROWS - 2;
    feed(&t, "\033[5B");
    ASSERT_EQ(t.cursor_row, GTERM_ROWS - 1, "cursor down clamps at last row");
}

static void test_ansi_cursor_right(void)
{
    gterm_t t = make_fresh();
    t.cursor_col = 5;
    feed(&t, "\033[C");
    ASSERT_EQ(t.cursor_col, 6, "\\033[C moves right 1");

    t.cursor_col = GTERM_COLS - 2;
    feed(&t, "\033[5C");
    ASSERT_EQ(t.cursor_col, GTERM_COLS - 1, "cursor right clamps at last col");
}

static void test_ansi_cursor_left(void)
{
    gterm_t t = make_fresh();
    t.cursor_col = 5;
    feed(&t, "\033[D");
    ASSERT_EQ(t.cursor_col, 4, "\\033[D moves left 1");

    t.cursor_col = 1;
    feed(&t, "\033[5D");
    ASSERT_EQ(t.cursor_col, 0, "cursor left clamps at col 0");
}

/* ── ANSI clear / erase ──────────────────────────────────────────────────── */

static void test_ansi_clear_screen(void)
{
    gterm_t t = make_fresh();
    feed(&t, "Hello");
    t.cursor_row = 5; t.cursor_col = 10;

    feed(&t, "\033[2J");

    ASSERT_EQ(t.cursor_row, 0, "\\033[2J homes cursor row");
    ASSERT_EQ(t.cursor_col, 0, "\\033[2J homes cursor col");

    /* All cells should be blank. */
    int blank = 1;
    int r, c;
    for (r = 0; r < GTERM_ROWS; r++)
        for (c = 0; c < GTERM_COLS; c++)
            if (t.cells[r][c].ch != ' ') { blank = 0; break; }
    ASSERT_EQ(blank, 1, "\\033[2J blanks all cells");
}

static void test_ansi_erase_line(void)
{
    gterm_t t = make_fresh();
    feed(&t, "ABCDE");
    t.cursor_col = 2;  /* cursor after 'C' */

    feed(&t, "\033[K");

    ASSERT_EQ(t.cells[0][0].ch, 'A', "A before cursor preserved");
    ASSERT_EQ(t.cells[0][1].ch, 'B', "B before cursor preserved");
    /* Cells from cursor_col onward should be spaces. */
    int erased = 1;
    int c;
    for (c = 2; c < GTERM_COLS; c++)
        if (t.cells[0][c].ch != ' ') { erased = 0; break; }
    ASSERT_EQ(erased, 1, "\\033[K erases from cursor to end of line");
    ASSERT_EQ(t.cursor_col, 2, "cursor col unchanged after \\033[K");
}

/* ── ANSI SGR (colours / attributes) ─────────────────────────────────────── */

static void test_ansi_sgr_reset(void)
{
    gterm_t t = make_fresh();
    /* Change the colour first */
    feed(&t, "\033[32m");   /* green fg */
    feed(&t, "\033[0m");    /* reset */
    ASSERT_EQ((int)t.cur_fg, (int)GT_DEFAULT_FG, "\\033[0m resets fg");
    ASSERT_EQ((int)t.cur_bg, (int)GT_DEFAULT_BG, "\\033[0m resets bg");
    ASSERT_EQ(t.bold, 0,                          "\\033[0m clears bold");
}

static void test_ansi_sgr_bold(void)
{
    gterm_t t = make_fresh();
    feed(&t, "\033[1m");
    ASSERT_EQ(t.bold, 1, "\\033[1m sets bold");

    /* Bold + colour 30 (black) should map to bright black (palette[8]). */
    feed(&t, "\033[30m");
    ASSERT_EQ((int)t.cur_fg, (int)gterm_palette[8], "bold+30 → bright black");
}

static void test_ansi_sgr_fg(void)
{
    gterm_t t = make_fresh();
    feed(&t, "\033[32m");   /* green */
    ASSERT_EQ((int)t.cur_fg, (int)gterm_palette[2], "\\033[32m → green");

    feed(&t, "\033[31m");   /* red */
    ASSERT_EQ((int)t.cur_fg, (int)gterm_palette[1], "\\033[31m → red");

    /* 39 = default fg */
    feed(&t, "\033[39m");
    ASSERT_EQ((int)t.cur_fg, (int)GT_DEFAULT_FG, "\\033[39m → default fg");
}

static void test_ansi_sgr_bg(void)
{
    gterm_t t = make_fresh();
    feed(&t, "\033[41m");   /* red bg */
    ASSERT_EQ((int)t.cur_bg, (int)gterm_palette[1], "\\033[41m → red bg");

    /* 49 = default bg */
    feed(&t, "\033[49m");
    ASSERT_EQ((int)t.cur_bg, (int)GT_DEFAULT_BG, "\\033[49m → default bg");
}

static void test_ansi_sgr_bright_fg(void)
{
    gterm_t t = make_fresh();
    feed(&t, "\033[92m");   /* bright green */
    ASSERT_EQ((int)t.cur_fg, (int)gterm_palette[10], "\\033[92m → bright green");
}

static void test_ansi_sgr_combined(void)
{
    gterm_t t = make_fresh();
    feed(&t, "\033[1;32m");   /* bold + green in one sequence */
    ASSERT_EQ(t.bold, 1, "\\033[1;32m sets bold");
    /* bold is set before colour 32 is processed, so idx = 2+8 = 10 */
    ASSERT_EQ((int)t.cur_fg, (int)gterm_palette[10],
              "\\033[1;32m sets bright green (bold+green)");
}

static void test_ansi_sgr_cells_inherit_colour(void)
{
    gterm_t t = make_fresh();
    feed(&t, "\033[32m");   /* green fg */
    gterm_write_char(&t, 'X');
    ASSERT_EQ((int)t.cells[0][0].fg, (int)gterm_palette[2],
              "written char inherits current fg colour");
}

/* ── ANSI robustness ─────────────────────────────────────────────────────── */

static void test_ansi_incomplete_esc(void)
{
    gterm_t t = make_fresh();
    /* ESC followed by a non-'[' byte: should be ignored, not crash. */
    feed(&t, "\033Z");
    ASSERT_EQ((int)t.ansi_state, (int)GTERM_ST_NORMAL, "returns to NORMAL after bad ESC");
    ASSERT_EQ(t.cursor_row, 0, "no cursor change from bad ESC");
    ASSERT_EQ(t.cursor_col, 0, "no cursor change from bad ESC");
}

static void test_ansi_overlong_param(void)
{
    gterm_t t = make_fresh();
    /* Send a CSI with more than GTERM_ANSI_BUF param bytes then a final byte.
     * Must not crash or corrupt memory.                                   */
    int i;
    gterm_write_char(&t, '\033');
    gterm_write_char(&t, '[');
    for (i = 0; i < GTERM_ANSI_BUF + 8; i++)
        gterm_write_char(&t, '0');
    gterm_write_char(&t, 'm');  /* SGR final byte */
    ASSERT_EQ((int)t.ansi_state, (int)GTERM_ST_NORMAL, "overlong CSI: returns to NORMAL");
}

/* ── gterm_write batch vs char-by-char ───────────────────────────────────── */

static void test_write_batch(void)
{
    gterm_t ta = make_fresh();
    gterm_t tb = make_fresh();

    const char *msg = "Hello\033[32mWorld\033[0m!";
    int len = (int)strlen(msg);

    gterm_write(&ta, msg, len);

    int i;
    for (i = 0; i < len; i++)
        gterm_write_char(&tb, msg[i]);

    /* Both should produce identical cell grids. */
    int same = (memcmp(ta.cells, tb.cells, sizeof(ta.cells)) == 0);
    ASSERT_EQ(same, 1, "gterm_write matches char-by-char for mixed ANSI");
}

/* ── Param parser ────────────────────────────────────────────────────────── */

static void test_param_parser(void)
{
    int params[GTERM_MAX_PARAMS];
    int i;

    /* Empty string → one param = 0 */
    for (i = 0; i < GTERM_MAX_PARAMS; i++) params[i] = -1;
    int n = gterm_parse_params("", params, GTERM_MAX_PARAMS);
    ASSERT_EQ(n, 1,          "empty string: 1 param");
    ASSERT_EQ(params[0], 0,  "empty string: param[0] = 0");

    /* Single integer */
    for (i = 0; i < GTERM_MAX_PARAMS; i++) params[i] = -1;
    n = gterm_parse_params("42", params, GTERM_MAX_PARAMS);
    ASSERT_EQ(n, 1,           "single int: 1 param");
    ASSERT_EQ(params[0], 42,  "single int: param[0] = 42");

    /* Two integers */
    for (i = 0; i < GTERM_MAX_PARAMS; i++) params[i] = -1;
    n = gterm_parse_params("5;12", params, GTERM_MAX_PARAMS);
    ASSERT_EQ(n, 2,           "two ints: 2 params");
    ASSERT_EQ(params[0], 5,   "two ints: param[0] = 5");
    ASSERT_EQ(params[1], 12,  "two ints: param[1] = 12");

    /* Trailing semicolon adds a zero param */
    for (i = 0; i < GTERM_MAX_PARAMS; i++) params[i] = -1;
    n = gterm_parse_params("1;", params, GTERM_MAX_PARAMS);
    ASSERT_EQ(n, 2,          "trailing ';': 2 params");
    ASSERT_EQ(params[0], 1,  "trailing ';': param[0] = 1");
    ASSERT_EQ(params[1], 0,  "trailing ';': param[1] = 0");

    /* Three integers */
    for (i = 0; i < GTERM_MAX_PARAMS; i++) params[i] = -1;
    n = gterm_parse_params("1;2;3", params, GTERM_MAX_PARAMS);
    ASSERT_EQ(n, 3,          "three ints: 3 params");
    ASSERT_EQ(params[2], 3,  "three ints: param[2] = 3");
}

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(void)
{
    RUN_SUITE(test_init);

    RUN_SUITE(test_plain_text);
    RUN_SUITE(test_control_newline);
    RUN_SUITE(test_control_cr);
    RUN_SUITE(test_control_backspace);

    RUN_SUITE(test_wrap);
    RUN_SUITE(test_scroll);

    RUN_SUITE(test_ansi_cursor_home);
    RUN_SUITE(test_ansi_cursor_position);
    RUN_SUITE(test_ansi_cursor_up);
    RUN_SUITE(test_ansi_cursor_down);
    RUN_SUITE(test_ansi_cursor_right);
    RUN_SUITE(test_ansi_cursor_left);

    RUN_SUITE(test_ansi_clear_screen);
    RUN_SUITE(test_ansi_erase_line);

    RUN_SUITE(test_ansi_sgr_reset);
    RUN_SUITE(test_ansi_sgr_bold);
    RUN_SUITE(test_ansi_sgr_fg);
    RUN_SUITE(test_ansi_sgr_bg);
    RUN_SUITE(test_ansi_sgr_bright_fg);
    RUN_SUITE(test_ansi_sgr_combined);
    RUN_SUITE(test_ansi_sgr_cells_inherit_colour);

    RUN_SUITE(test_ansi_incomplete_esc);
    RUN_SUITE(test_ansi_overlong_param);

    RUN_SUITE(test_write_batch);
    RUN_SUITE(test_param_parser);

    TEST_SUMMARY();
}
