#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdint.h>

#define KEYBOARD_BUFFER_SIZE 256

void keyboard_initialize(void);
void keyboard_handle_scancode(uint8_t scancode);
char keyboard_getchar(void);
int  keyboard_available(void);

#endif
