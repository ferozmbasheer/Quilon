/*
 * test_ansi.c — host-side unit tests for the ANSI escape sequence parser.
 *
 * Tests the pure-C functions in kernel/include/kernel/ansi.h:
 *   - ansi_parser_init()
 *   - ansi_feed() state machine
 *   - ansi_parse_params()
 *   - ansi_sgr_color()
 *
 * No kernel headers, no x86 asm, no hardware dependencies.
 */

#include "framework.h"
#include "../kernel/include/kernel/ansi.h"

/* ── Helper: feed a NUL-terminated string, count results ───────────────── */

typedef struct {
    int  chars;         /* number of ANSI_CHAR events */
    int  csi;           /* number of ANSI_CSI events  */
    char last_ch;       /* last ANSI_CHAR character    */
    char last_cmd;      /* last ANSI_CSI command byte  */
    int  last_params[ANSI_MAX_PARAMS];
    int  last_nparams;
} feed_result_t;

static feed_result_t feed_string(const char *s)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    feed_result_t r = {0, 0, 0, 0, {0}, 0};

    for (const char *c = s; *c; c++) {
        ansi_event_t ev;
        int res = ansi_feed(&p, *c, &ev);
        if (res == ANSI_CHAR) {
            r.chars++;
            r.last_ch = ev.ch;
        } else if (res == ANSI_CSI) {
            r.csi++;
            r.last_cmd    = ev.cmd;
            r.last_nparams = ev.nparams;
            for (int i = 0; i < ev.nparams && i < ANSI_MAX_PARAMS; i++)
                r.last_params[i] = ev.params[i];
        }
    }
    return r;
}

/* ── ansi_parser_init ──────────────────────────────────────────────────── */

static void test_parser_init(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ASSERT_EQ((int)p.state, (int)ANSI_STATE_NORMAL, "init: state is NORMAL");
    ASSERT_EQ(p.len, 0, "init: len is 0");
}

/* ── ansi_feed — plain characters ─────────────────────────────────────── */

static void test_plain_chars(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    int r;

    r = ansi_feed(&p, 'A', &ev);
    ASSERT_EQ(r, ANSI_CHAR, "plain 'A' → ANSI_CHAR");
    ASSERT_EQ((int)ev.ch, (int)'A', "plain 'A' → ev.ch=='A'");

    r = ansi_feed(&p, '\n', &ev);
    ASSERT_EQ(r, ANSI_CHAR, "newline → ANSI_CHAR");
    ASSERT_EQ((int)ev.ch, (int)'\n', "newline → ev.ch==\\n");

    r = ansi_feed(&p, '\r', &ev);
    ASSERT_EQ(r, ANSI_CHAR, "CR → ANSI_CHAR");

    r = ansi_feed(&p, '\b', &ev);
    ASSERT_EQ(r, ANSI_CHAR, "backspace → ANSI_CHAR");
}

/* ── ansi_feed — ESC alone is consumed ────────────────────────────────── */

static void test_esc_alone(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    int r = ansi_feed(&p, '\033', &ev);
    ASSERT_EQ(r, ANSI_NONE, "ESC alone → ANSI_NONE");
    ASSERT_EQ((int)p.state, (int)ANSI_STATE_ESC, "ESC → state is SAW_ESC");
}

/* ── ansi_feed — ESC + non-[ emits char literally ─────────────────────── */

static void test_esc_non_bracket(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    ansi_feed(&p, '\033', &ev);  /* ESC */

    int r = ansi_feed(&p, 'c', &ev);
    ASSERT_EQ(r, ANSI_CHAR, "ESC+c → ANSI_CHAR (reset sequence, emit 'c')");
    ASSERT_EQ((int)p.state, (int)ANSI_STATE_NORMAL, "ESC+c → state restored to NORMAL");
}

/* ── ansi_feed — ESC [ alone does not complete ────────────────────────── */

static void test_csi_partial(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    ASSERT_EQ(ansi_feed(&p, '\033', &ev), ANSI_NONE, "ESC → ANSI_NONE");
    ASSERT_EQ(ansi_feed(&p, '[', &ev),   ANSI_NONE, "ESC[ → ANSI_NONE");
    ASSERT_EQ((int)p.state, (int)ANSI_STATE_CSI, "ESC[ → state is SAW_CSI");
    ASSERT_EQ(p.len, 0, "ESC[ → buf empty");
}

/* ── ansi_feed — ESC [ m (SGR reset, no params) ───────────────────────── */

static void test_sgr_reset(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    ansi_feed(&p, '\033', &ev);
    ansi_feed(&p, '[', &ev);
    int r = ansi_feed(&p, 'm', &ev);

    ASSERT_EQ(r, ANSI_CSI, "ESC[m → ANSI_CSI");
    ASSERT_EQ((int)ev.cmd, (int)'m', "ESC[m → cmd=='m'");
    ASSERT_EQ(ev.nparams, 0, "ESC[m → nparams==0 (no params)");
    ASSERT_EQ((int)p.state, (int)ANSI_STATE_NORMAL, "after CSI → state NORMAL");
}

/* ── ansi_feed — ESC [ 0 m (SGR reset with explicit 0) ───────────────── */

static void test_sgr_explicit_reset(void)
{
    feed_result_t r = feed_string("\033[0m");
    ASSERT_EQ(r.csi, 1, "ESC[0m → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'m', "ESC[0m → cmd=='m'");
    ASSERT_EQ(r.last_nparams, 1, "ESC[0m → nparams==1");
    ASSERT_EQ(r.last_params[0], 0, "ESC[0m → params[0]==0");
}

/* ── ansi_feed — ESC [ 31 m (red foreground) ─────────────────────────── */

static void test_sgr_color(void)
{
    feed_result_t r = feed_string("\033[31m");
    ASSERT_EQ(r.csi, 1, "ESC[31m → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'m', "ESC[31m → cmd=='m'");
    ASSERT_EQ(r.last_nparams, 1, "ESC[31m → nparams==1");
    ASSERT_EQ(r.last_params[0], 31, "ESC[31m → params[0]==31");
}

/* ── ansi_feed — ESC [ 1 ; 32 m (bold green) ─────────────────────────── */

static void test_sgr_bold_green(void)
{
    feed_result_t r = feed_string("\033[1;32m");
    ASSERT_EQ(r.csi, 1, "ESC[1;32m → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'m', "ESC[1;32m → cmd=='m'");
    ASSERT_EQ(r.last_nparams, 2, "ESC[1;32m → nparams==2");
    ASSERT_EQ(r.last_params[0], 1, "ESC[1;32m → params[0]==1 (bold)");
    ASSERT_EQ(r.last_params[1], 32, "ESC[1;32m → params[1]==32 (green)");
}

/* ── ansi_feed — ESC [ 2 J (clear screen) ────────────────────────────── */

static void test_erase_display(void)
{
    feed_result_t r = feed_string("\033[2J");
    ASSERT_EQ(r.csi, 1, "ESC[2J → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'J', "ESC[2J → cmd=='J'");
    ASSERT_EQ(r.last_nparams, 1, "ESC[2J → nparams==1");
    ASSERT_EQ(r.last_params[0], 2, "ESC[2J → params[0]==2");
}

/* ── ansi_feed — ESC [ K (erase to EOL, no param) ────────────────────── */

static void test_erase_line_no_param(void)
{
    feed_result_t r = feed_string("\033[K");
    ASSERT_EQ(r.csi, 1, "ESC[K → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'K', "ESC[K → cmd=='K'");
    ASSERT_EQ(r.last_nparams, 0, "ESC[K → nparams==0");
}

/* ── ansi_feed — ESC [ 0 K (erase to EOL, explicit 0) ───────────────── */

static void test_erase_line_explicit(void)
{
    feed_result_t r = feed_string("\033[0K");
    ASSERT_EQ(r.csi, 1, "ESC[0K → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'K', "ESC[0K → cmd=='K'");
    ASSERT_EQ(r.last_params[0], 0, "ESC[0K → params[0]==0");
}

/* ── ansi_feed — ESC [ 10 ; 5 H (cursor position) ───────────────────── */

static void test_cursor_position(void)
{
    feed_result_t r = feed_string("\033[10;5H");
    ASSERT_EQ(r.csi, 1, "ESC[10;5H → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'H', "ESC[10;5H → cmd=='H'");
    ASSERT_EQ(r.last_nparams, 2, "ESC[10;5H → nparams==2");
    ASSERT_EQ(r.last_params[0], 10, "ESC[10;5H → params[0]==10");
    ASSERT_EQ(r.last_params[1], 5,  "ESC[10;5H → params[1]==5");
}

/* ── ansi_feed — ESC [ H (cursor home, no params) ────────────────────── */

static void test_cursor_home(void)
{
    feed_result_t r = feed_string("\033[H");
    ASSERT_EQ(r.csi, 1, "ESC[H → 1 CSI event");
    ASSERT_EQ((int)r.last_cmd, (int)'H', "ESC[H → cmd=='H'");
    ASSERT_EQ(r.last_nparams, 0, "ESC[H → nparams==0");
}

/* ── ansi_feed — cursor movement A/B/C/D ─────────────────────────────── */

static void test_cursor_movement(void)
{
    feed_result_t up    = feed_string("\033[3A");
    feed_result_t down  = feed_string("\033[5B");
    feed_result_t right = feed_string("\033[2C");
    feed_result_t left  = feed_string("\033[4D");

    ASSERT_EQ((int)up.last_cmd,    (int)'A', "ESC[3A → cmd=='A'");
    ASSERT_EQ(up.last_params[0],   3,         "ESC[3A → params[0]==3");
    ASSERT_EQ((int)down.last_cmd,  (int)'B', "ESC[5B → cmd=='B'");
    ASSERT_EQ(down.last_params[0], 5,         "ESC[5B → params[0]==5");
    ASSERT_EQ((int)right.last_cmd, (int)'C', "ESC[2C → cmd=='C'");
    ASSERT_EQ(right.last_params[0],2,         "ESC[2C → params[0]==2");
    ASSERT_EQ((int)left.last_cmd,  (int)'D', "ESC[4D → cmd=='D'");
    ASSERT_EQ(left.last_params[0], 4,         "ESC[4D → params[0]==4");
}

/* ── ansi_feed — save/restore cursor ─────────────────────────────────── */

static void test_save_restore(void)
{
    feed_result_t save    = feed_string("\033[s");
    feed_result_t restore = feed_string("\033[u");
    ASSERT_EQ((int)save.last_cmd,    (int)'s', "ESC[s → cmd=='s'");
    ASSERT_EQ((int)restore.last_cmd, (int)'u', "ESC[u → cmd=='u'");
}

/* ── ansi_feed — multiple sequences in a row ─────────────────────────── */

static void test_multiple_sequences(void)
{
    feed_result_t r = feed_string("A\033[31mB\033[0mC");
    /* chars: 'A', 'B', 'C' = 3; CSI events: 2 */
    ASSERT_EQ(r.chars, 3, "3 chars in mixed string");
    ASSERT_EQ(r.csi,   2, "2 CSI events in mixed string");
    ASSERT_EQ((int)r.last_ch, (int)'C', "last char is 'C'");
    ASSERT_EQ((int)r.last_cmd, (int)'m', "last CSI cmd is 'm'");
}

/* ── ansi_feed — sequence does not bleed into next char ──────────────── */

static void test_no_bleed(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    /* Feed ESC[31m then 'X' */
    const char *seq = "\033[31m";
    for (const char *c = seq; *c; c++)
        ansi_feed(&p, *c, &ev);

    int r = ansi_feed(&p, 'X', &ev);
    ASSERT_EQ(r, ANSI_CHAR, "char after CSI sequence is ANSI_CHAR");
    ASSERT_EQ((int)ev.ch, (int)'X', "char after CSI is 'X'");
    ASSERT_EQ((int)p.state, (int)ANSI_STATE_NORMAL, "state is NORMAL after sequence+char");
}

/* ── ansi_parse_params ────────────────────────────────────────────────── */

static void test_parse_params_empty(void)
{
    int params[8];
    int n = ansi_parse_params("", 0, params, 8);
    ASSERT_EQ(n, 0, "empty string → 0 params");
}

static void test_parse_params_single(void)
{
    int params[8];
    int n = ansi_parse_params("0", 1, params, 8);
    ASSERT_EQ(n, 1, "\"0\" → 1 param");
    ASSERT_EQ(params[0], 0, "\"0\" → params[0]==0");

    n = ansi_parse_params("31", 2, params, 8);
    ASSERT_EQ(n, 1, "\"31\" → 1 param");
    ASSERT_EQ(params[0], 31, "\"31\" → params[0]==31");

    n = ansi_parse_params("100", 3, params, 8);
    ASSERT_EQ(n, 1, "\"100\" → 1 param");
    ASSERT_EQ(params[0], 100, "\"100\" → params[0]==100");
}

static void test_parse_params_multiple(void)
{
    int params[8];
    int n = ansi_parse_params("1;32", 4, params, 8);
    ASSERT_EQ(n, 2, "\"1;32\" → 2 params");
    ASSERT_EQ(params[0], 1,  "\"1;32\" → params[0]==1");
    ASSERT_EQ(params[1], 32, "\"1;32\" → params[1]==32");

    n = ansi_parse_params("10;5;0", 6, params, 8);
    ASSERT_EQ(n, 3, "\"10;5;0\" → 3 params");
    ASSERT_EQ(params[0], 10, "\"10;5;0\" → params[0]==10");
    ASSERT_EQ(params[1], 5,  "\"10;5;0\" → params[1]==5");
    ASSERT_EQ(params[2], 0,  "\"10;5;0\" → params[2]==0");
}

static void test_parse_params_empty_fields(void)
{
    int params[8];
    /* Leading semicolon: one field of 0, then "32" */
    int n = ansi_parse_params(";32", 3, params, 8);
    ASSERT_EQ(n, 2, "\";32\" → 2 params");
    ASSERT_EQ(params[0], 0,  "\";32\" → params[0]==0 (empty first field)");
    ASSERT_EQ(params[1], 32, "\";32\" → params[1]==32");
}

static void test_parse_params_trailing_semicolon(void)
{
    int params[8];
    /* Trailing semicolon: just one param "1" */
    int n = ansi_parse_params("1;", 2, params, 8);
    ASSERT_EQ(n, 1, "\"1;\" → 1 param (trailing semicolon ignored)");
    ASSERT_EQ(params[0], 1, "\"1;\" → params[0]==1");
}

static void test_parse_params_max_cap(void)
{
    int params[8];
    /* More params than max_params=2: should truncate */
    int n = ansi_parse_params("1;2;3;4", 7, params, 2);
    ASSERT_EQ(n, 2, "truncated to max_params=2");
    ASSERT_EQ(params[0], 1, "first param preserved");
    ASSERT_EQ(params[1], 2, "second param preserved");
}

/* ── ansi_sgr_color ───────────────────────────────────────────────────── */

static void test_sgr_color_standard(void)
{
    /* Standard 30-37 (foreground) */
    ASSERT_EQ((int)ansi_sgr_color(30), (int)0x000000u, "30=black");
    ASSERT_EQ((int)ansi_sgr_color(31), (int)0xAA0000u, "31=red");
    ASSERT_EQ((int)ansi_sgr_color(32), (int)0x00AA00u, "32=green");
    ASSERT_EQ((int)ansi_sgr_color(33), (int)0xAAAA00u, "33=yellow");
    ASSERT_EQ((int)ansi_sgr_color(34), (int)0x0000AAu, "34=blue");
    ASSERT_EQ((int)ansi_sgr_color(35), (int)0xAA00AAu, "35=magenta");
    ASSERT_EQ((int)ansi_sgr_color(36), (int)0x00AAAAu, "36=cyan");
    ASSERT_EQ((int)ansi_sgr_color(37), (int)0xAAAAAAu, "37=white");
}

static void test_sgr_color_background(void)
{
    /* Background 40-47: same palette as foreground */
    ASSERT_EQ((int)ansi_sgr_color(40), (int)ansi_sgr_color(30), "40 == 30 palette");
    ASSERT_EQ((int)ansi_sgr_color(41), (int)ansi_sgr_color(31), "41 == 31 palette");
    ASSERT_EQ((int)ansi_sgr_color(42), (int)ansi_sgr_color(32), "42 == 32 palette");
    ASSERT_EQ((int)ansi_sgr_color(47), (int)ansi_sgr_color(37), "47 == 37 palette");
}

static void test_sgr_color_bright(void)
{
    /* Bright 90-97 */
    ASSERT_EQ((int)ansi_sgr_color(90), (int)0x555555u, "90=bright black");
    ASSERT_EQ((int)ansi_sgr_color(91), (int)0xFF5555u, "91=bright red");
    ASSERT_EQ((int)ansi_sgr_color(92), (int)0x55FF55u, "92=bright green");
    ASSERT_EQ((int)ansi_sgr_color(97), (int)0xFFFFFFu, "97=bright white");
}

static void test_sgr_color_bright_bg(void)
{
    /* Bright background 100-107: same as 90-97 */
    ASSERT_EQ((int)ansi_sgr_color(100), (int)ansi_sgr_color(90),  "100 == 90 palette");
    ASSERT_EQ((int)ansi_sgr_color(101), (int)ansi_sgr_color(91),  "101 == 91 palette");
    ASSERT_EQ((int)ansi_sgr_color(107), (int)ansi_sgr_color(97),  "107 == 97 palette");
}

static void test_sgr_color_invalid(void)
{
    ASSERT_EQ((int)ansi_sgr_color(0),   (int)0xFFFFFFFFu, "0 is invalid");
    ASSERT_EQ((int)ansi_sgr_color(29),  (int)0xFFFFFFFFu, "29 is invalid");
    ASSERT_EQ((int)ansi_sgr_color(38),  (int)0xFFFFFFFFu, "38 is invalid");
    ASSERT_EQ((int)ansi_sgr_color(48),  (int)0xFFFFFFFFu, "48 is invalid");
    ASSERT_EQ((int)ansi_sgr_color(89),  (int)0xFFFFFFFFu, "89 is invalid");
    ASSERT_EQ((int)ansi_sgr_color(108), (int)0xFFFFFFFFu, "108 is invalid");
    ASSERT_EQ((int)ansi_sgr_color(-1),  (int)0xFFFFFFFFu, "-1 is invalid");
}

static void test_sgr_colors_unique(void)
{
    /* The 8 standard colors must all be distinct values */
    int all_unique = 1;
    for (int i = 30; i <= 37; i++)
        for (int j = i + 1; j <= 37; j++)
            if (ansi_sgr_color(i) == ansi_sgr_color(j)) all_unique = 0;
    ASSERT_NE(all_unique, 0, "standard colors 30-37 are all distinct");

    /* The 8 bright colors must all be distinct */
    all_unique = 1;
    for (int i = 90; i <= 97; i++)
        for (int j = i + 1; j <= 97; j++)
            if (ansi_sgr_color(i) == ansi_sgr_color(j)) all_unique = 0;
    ASSERT_NE(all_unique, 0, "bright colors 90-97 are all distinct");
}

/* ── ansi_feed — full round-trip: chars preserved around CSI ─────────── */

static void test_round_trip_chars(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    /* "Hello" → 5 chars, no CSI */
    char buf[8];
    int  n = 0;
    const char *hello = "Hello";
    for (const char *c = hello; *c; c++) {
        if (ansi_feed(&p, *c, &ev) == ANSI_CHAR)
            buf[n++] = ev.ch;
    }
    buf[n] = '\0';
    ASSERT_STR_EQ(buf, "Hello", "plain string round-trips through parser");
}

static void test_round_trip_mixed(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    /* "\033[31mHi\033[0m" → chars 'H','i' and 2 CSI events */
    const char *mixed = "\033[31mHi\033[0m";
    int chars = 0, csi = 0;
    char cbuf[4];
    for (const char *c = mixed; *c; c++) {
        int r = ansi_feed(&p, *c, &ev);
        if (r == ANSI_CHAR) cbuf[chars++] = ev.ch;
        if (r == ANSI_CSI)  csi++;
    }
    cbuf[chars] = '\0';
    ASSERT_EQ(chars, 2, "2 chars in mixed string");
    ASSERT_EQ(csi,   2, "2 CSI events in mixed string");
    ASSERT_STR_EQ(cbuf, "Hi", "chars are 'H' and 'i'");
}

/* ── ANSI_BUF_SIZE overflow: long param string truncated, still terminates */

static void test_long_param_no_hang(void)
{
    ansi_parser_t p;
    ansi_parser_init(&p);
    ansi_event_t ev;

    /* Feed a very long param string (longer than ANSI_BUF_SIZE) then a final byte */
    ansi_feed(&p, '\033', &ev);
    ansi_feed(&p, '[', &ev);
    for (int i = 0; i < ANSI_BUF_SIZE + 10; i++)
        ansi_feed(&p, '1', &ev);  /* lots of '1' digits */
    int r = ansi_feed(&p, 'm', &ev);

    ASSERT_EQ(r, ANSI_CSI, "long param string still completes on final byte");
    ASSERT_EQ((int)ev.cmd, (int)'m', "long param → cmd=='m'");
    ASSERT_EQ((int)p.state, (int)ANSI_STATE_NORMAL, "state reset after long param");
}

/* ── Constants ───────────────────────────────────────────────────────────── */

static void test_constants(void)
{
    ASSERT_EQ(ANSI_NONE, 0, "ANSI_NONE == 0");
    ASSERT_NE(ANSI_CHAR, ANSI_NONE, "ANSI_CHAR != ANSI_NONE");
    ASSERT_NE(ANSI_CSI,  ANSI_NONE, "ANSI_CSI != ANSI_NONE");
    ASSERT_NE(ANSI_CSI,  ANSI_CHAR, "ANSI_CSI != ANSI_CHAR");
    ASSERT_NE(ANSI_MAX_PARAMS, 0, "ANSI_MAX_PARAMS > 0");
    ASSERT_NE(ANSI_BUF_SIZE,   0, "ANSI_BUF_SIZE > 0");
}

/* ── main ───────────────────────────────────────────────────────────────── */

int main(void)
{
    RUN_SUITE(test_constants);
    RUN_SUITE(test_parser_init);
    RUN_SUITE(test_plain_chars);
    RUN_SUITE(test_esc_alone);
    RUN_SUITE(test_esc_non_bracket);
    RUN_SUITE(test_csi_partial);
    RUN_SUITE(test_sgr_reset);
    RUN_SUITE(test_sgr_explicit_reset);
    RUN_SUITE(test_sgr_color);
    RUN_SUITE(test_sgr_bold_green);
    RUN_SUITE(test_erase_display);
    RUN_SUITE(test_erase_line_no_param);
    RUN_SUITE(test_erase_line_explicit);
    RUN_SUITE(test_cursor_position);
    RUN_SUITE(test_cursor_home);
    RUN_SUITE(test_cursor_movement);
    RUN_SUITE(test_save_restore);
    RUN_SUITE(test_multiple_sequences);
    RUN_SUITE(test_no_bleed);
    RUN_SUITE(test_parse_params_empty);
    RUN_SUITE(test_parse_params_single);
    RUN_SUITE(test_parse_params_multiple);
    RUN_SUITE(test_parse_params_empty_fields);
    RUN_SUITE(test_parse_params_trailing_semicolon);
    RUN_SUITE(test_parse_params_max_cap);
    RUN_SUITE(test_sgr_color_standard);
    RUN_SUITE(test_sgr_color_background);
    RUN_SUITE(test_sgr_color_bright);
    RUN_SUITE(test_sgr_color_bright_bg);
    RUN_SUITE(test_sgr_color_invalid);
    RUN_SUITE(test_sgr_colors_unique);
    RUN_SUITE(test_round_trip_chars);
    RUN_SUITE(test_round_trip_mixed);
    RUN_SUITE(test_long_param_no_hang);

    TEST_SUMMARY();
}
