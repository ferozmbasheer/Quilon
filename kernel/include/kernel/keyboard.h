#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_BUFFER_SIZE 256

void keyboard_initialize(void);
void keyboard_handle_scancode(uint8_t scancode);
char keyboard_getchar(void);
int  keyboard_available(void);

/* Like keyboard_getchar but returns -1 (instead of sleeping on) when a
 * signal is pending for the caller; 0 with *out set on success. */
int  keyboard_getchar_intr(char *out);

/* Register the Ctrl-C hook (IRQ context).  It returns 1 if it signalled a
 * foreground job -- the 0x03 byte is then not buffered. */
void keyboard_set_intr_handler(int (*fn)(void));

#endif
