/*
 * Quilon OS -- scancode translation tests (ROADMAP3 17.0.1)
 *
 * Drives kernel/kernel/kbd_xlate.c with raw set-1 scancode bytes and checks
 * the byte stream a terminal program would read.
 */

#include "framework.h"
#include <string.h>
#include <kernel/kbd_xlate.h>

/* Feed bytes; return everything emitted, NUL-terminated, in a static buffer. */
static char outbuf[128];
static const char *feed(kbd_state_t *st, const uint8_t *sc, int n)
{
    int len = 0;
    for (int i = 0; i < n; i++) {
        char tmp[KBD_SEQ_MAX];
        int k = kbd_translate(st, sc[i], tmp);
        for (int j = 0; j < k && len < (int)sizeof outbuf - 1; j++)
            outbuf[len++] = tmp[j];
    }
    outbuf[len] = '\0';
    return outbuf;
}
#define FEED(st, ...) \
    feed((st), (const uint8_t[]){__VA_ARGS__}, \
         (int)sizeof((const uint8_t[]){__VA_ARGS__}))

static void test_plain_keys(void)
{
    kbd_state_t st; kbd_state_init(&st);
    ASSERT_STR_EQ(FEED(&st, 0x1E, 0x9E, 0x30, 0xB0), "ab", "a, b (with releases)");
    ASSERT_STR_EQ(FEED(&st, 0x02, 0x0B, 0x0C, 0x0D), "10-=", "digits and - =");
    ASSERT_STR_EQ(FEED(&st, 0x1C), "\n", "Enter -> \\n");
    ASSERT_STR_EQ(FEED(&st, 0x0E), "\b", "Backspace -> \\b");
    ASSERT_STR_EQ(FEED(&st, 0x01), "\x1b", "Esc");
    ASSERT_STR_EQ(FEED(&st, 0x39), " ", "space");
    ASSERT_STR_EQ(FEED(&st, 0x9E), "", "release alone produces nothing");
}

static void test_shift(void)
{
    kbd_state_t st; kbd_state_init(&st);
    /* LShift down, 9 0 ' ; [ ] , . / ` - =, LShift up, a */
    ASSERT_STR_EQ(FEED(&st, 0x2A, 0x0A, 0x0B, 0x28, 0x27, 0x1A, 0x1B,
                       0x33, 0x34, 0x35, 0x29, 0x0C, 0x0D, 0x2B, 0x08,
                       0x09, 0x04, 0x03, 0xAA, 0x1E),
                  "()\":{}<>?~_+|&*#@a", "shifted symbols, then release");
    ASSERT_STR_EQ(FEED(&st, 0x36, 0x1E, 0xB6, 0x1E), "Aa", "right shift");
    /* Both shifts down, release one: still shifted. */
    ASSERT_STR_EQ(FEED(&st, 0x2A, 0x36, 0xAA, 0x1E, 0xB6, 0x1E), "Aa",
                  "shift held while the other is released");
}

static void test_caps_lock(void)
{
    kbd_state_t st; kbd_state_init(&st);
    ASSERT_STR_EQ(FEED(&st, 0x3A, 0xBA, 0x1E, 0x02), "A1", "caps: letters only");
    ASSERT_STR_EQ(FEED(&st, 0x2A, 0x1E, 0x02, 0xAA), "a!", "caps + shift inverts");
    ASSERT_STR_EQ(FEED(&st, 0x3A, 0xBA, 0x1E), "a", "caps toggles off");
}

static void test_ctrl(void)
{
    kbd_state_t st; kbd_state_init(&st);
    ASSERT_STR_EQ(FEED(&st, 0x1D, 0x2E, 0x9D), "\x03", "Ctrl-C = 0x03");
    ASSERT_STR_EQ(FEED(&st, 0x1D, 0x1E, 0x2C, 0x9D), "\x01\x1a", "Ctrl-A, Ctrl-Z");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x1D, 0x20, 0xE0, 0x9D, 0x20), "\x04" "d",
                  "right Ctrl (E0 1D) and its release");
    ASSERT_STR_EQ(FEED(&st, 0x1D, 0x1A, 0x9D), "\x1b", "Ctrl-[ = ESC");
    /* Shift does not change the Ctrl code; caps neither. */
    ASSERT_STR_EQ(FEED(&st, 0x3A, 0x2A, 0x1D, 0x2E, 0x9D, 0xAA, 0x3A),
                  "\x03", "Ctrl-C with shift+caps still 0x03");
}

static void test_alt(void)
{
    kbd_state_t st; kbd_state_init(&st);
    ASSERT_STR_EQ(FEED(&st, 0x38, 0x2D, 0xB8, 0x2D), "\x1bxx", "Alt-x = ESC x");
}

static void test_extended(void)
{
    kbd_state_t st; kbd_state_init(&st);
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x48, 0xE0, 0xC8), "\x1b[A", "Up");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x50), "\x1b[B", "Down");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x4D), "\x1b[C", "Right");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x4B), "\x1b[D", "Left");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x47, 0xE0, 0x4F), "\x1b[H\x1b[F", "Home End");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x53), "\x1b[3~", "Delete");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x49, 0xE0, 0x51), "\x1b[5~\x1b[6~", "PgUp PgDn");
    ASSERT_STR_EQ(FEED(&st, 0x3B, 0x3F, 0x58), "\x1bOP\x1b[15~\x1b[24~", "F1 F5 F12");
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x1C, 0xE0, 0x35), "\n/", "keypad Enter and /");
    ASSERT_STR_EQ(FEED(&st, 0x4A, 0x4E, 0x37), "-+*", "keypad - + *");
    /* An E0 prefix must not leak onto the next key. */
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x48, 0x1E), "\x1b[Aa", "prefix consumed");
}

/* ROADMAP3 18.2: modifiers on special keys use the xterm parameter forms. */
static void test_modified_specials(void)
{
    kbd_state_t st; kbd_state_init(&st);
    /* Shift (0x2A) + Up, release Shift. */
    ASSERT_STR_EQ(FEED(&st, 0x2A, 0xE0, 0x48, 0xAA), "\x1b[1;2A", "Shift-Up");
    /* Alt (0x38) + F4 (0x3E): the WM's close-window shortcut. */
    ASSERT_STR_EQ(FEED(&st, 0x38, 0x3E, 0xB8), "\x1b[1;3S", "Alt-F4");
    /* Ctrl (0x1D) + Delete. */
    ASSERT_STR_EQ(FEED(&st, 0x1D, 0xE0, 0x53, 0x9D), "\x1b[3;5~", "Ctrl-Delete");
    /* Ctrl + Alt + Shift + F12: the longest sequence. */
    ASSERT_STR_EQ(FEED(&st, 0x1D, 0x38, 0x2A, 0x58, 0xAA, 0xB8, 0x9D),
                  "\x1b[24;8~", "Ctrl-Alt-Shift-F12");
    ASSERT((int)strlen("\x1b[24;8~") <= KBD_SEQ_MAX, "fits KBD_SEQ_MAX");
    /* Right Ctrl (E0 1D) + Left. */
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x1D, 0xE0, 0x4B, 0xE0, 0x9D), "\x1b[1;5D",
                  "RCtrl-Left");
    /* Released modifiers: back to the plain forms. */
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x48, 0x3E), "\x1b[A\x1bOS", "plain again");
    /* Alt-Tab is still ESC TAB (not a special key). */
    ASSERT_STR_EQ(FEED(&st, 0x38, 0x0F, 0xB8), "\x1b\t", "Alt-Tab");
}

static void test_fake_shift_and_pause(void)
{
    kbd_state_t st; kbd_state_init(&st);
    /* Print Screen: E0 2A E0 37 -- the fake shift must not stick. */
    ASSERT_STR_EQ(FEED(&st, 0xE0, 0x2A, 0xE0, 0x37, 0xE0, 0xB7, 0xE0, 0xAA, 0x1E),
                  "a", "PrtSc fake shift ignored");
    /* Pause: E1 1D 45 E1 9D C5 -- must not leave Ctrl held. */
    ASSERT_STR_EQ(FEED(&st, 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5, 0x2E), "c",
                  "Pause swallowed, no stuck Ctrl");
}

int main(void)
{
    RUN_SUITE(test_plain_keys);
    RUN_SUITE(test_shift);
    RUN_SUITE(test_caps_lock);
    RUN_SUITE(test_ctrl);
    RUN_SUITE(test_alt);
    RUN_SUITE(test_extended);
    RUN_SUITE(test_modified_specials);
    RUN_SUITE(test_fake_shift_and_pause);
    TEST_SUMMARY();
}
