#include <stdint.h>
#include <kernel/keyboard.h>
#include <kernel/waitq.h>
#include <kernel/kbd_xlate.h>
#ifdef __is_kernel
#include <kernel/process.h>
#include <kernel/signal.h>
#endif

/* Scancode -> byte translation (shift, caps, ctrl, alt, extended keys) lives
 * in kbd_xlate.c so it can be unit-tested on the host (tests/test_kbd.c). */
static kbd_state_t       kb_state;

/* Called from IRQ context on Ctrl-C; returns 1 if it delivered SIGINT to a
 * foreground job (the 0x03 byte is then dropped), 0 to buffer it as data. */
static int             (*kb_intr_handler)(void);

static char              kb_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint8_t  kb_read_pos  = 0;
static volatile uint8_t  kb_write_pos = 0;
static waitq_t           kb_wq        = WAITQ_INIT;

void keyboard_initialize(void) {
    kb_read_pos  = 0;
    kb_write_pos = 0;
    kb_wq        = (waitq_t)WAITQ_INIT;
    kbd_state_init(&kb_state);
}

void keyboard_set_intr_handler(int (*fn)(void)) {
    kb_intr_handler = fn;
}

void keyboard_handle_scancode(uint8_t scancode) {
    char seq[KBD_SEQ_MAX];
    int  n = kbd_translate(&kb_state, scancode, seq);
    if (n <= 0)
        return;

    /* Ctrl-C: interrupt the console's foreground job instead of buffering. */
    if (n == 1 && seq[0] == 0x03 && kb_intr_handler && kb_intr_handler())
        return;

    /* Buffer the whole sequence or none of it: a reader must never see half
     * of an escape sequence (ESC [ without its final byte). */
    int used = (kb_write_pos + KEYBOARD_BUFFER_SIZE - kb_read_pos)
               % KEYBOARD_BUFFER_SIZE;
    if (used + n > KEYBOARD_BUFFER_SIZE - 1)
        return; /* buffer full -- drop the key */

    for (int i = 0; i < n; i++) {
        kb_buffer[kb_write_pos] = seq[i];
        kb_write_pos = (uint8_t)((kb_write_pos + 1) % KEYBOARD_BUFFER_SIZE);
    }

    /* Wake one process blocked in keyboard_getchar. */
    waitq_wake_one(&kb_wq);
}

int keyboard_available(void) {
    return kb_read_pos != kb_write_pos;
}

char keyboard_getchar(void) {
    /* Sleep until a character is available.  waitq_sleep is a no-op
     * when current_process is NULL (early boot), so the loop degrades
     * to a busy-wait in that case. */
    while (!keyboard_available())
        waitq_sleep(&kb_wq);

    char c = kb_buffer[kb_read_pos];
    kb_read_pos = (kb_read_pos + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

/*
 * keyboard_getchar_intr -- like keyboard_getchar, but gives up if a signal
 * becomes pending for the calling process (Ctrl-C, kill).  Returns 0 and
 * stores the character, or -1 if interrupted.  SYS_READ uses this so a
 * process blocked reading the keyboard can be killed: signal_send() readies
 * it, and the syscall returns so signal_dispatch() runs on the way out.
 */
int keyboard_getchar_intr(char *out) {
    for (;;) {
        if (keyboard_available()) {
            *out = keyboard_getchar();
            return 0;
        }
#ifdef __is_kernel
        if (current_process && signal_pending(current_process))
            return -1;
#endif
        waitq_sleep(&kb_wq);
    }
}
