#include <stdint.h>
#include <kernel/keyboard.h>

static const char keyboard_map[128] = {
      0,  27, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', '0', '-', '=', '\b',
    '\t',
    'q', 'w', 'e', 'r',
    't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
      0,           /* 29 - Control */
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
    '\'', '`',  0, /* Left shift */
    '\\', 'z', 'x', 'c', 'v', 'b', 'n',
    'm', ',', '.', '/',  0,  /* Right shift */
    '*',
      0,  /* Alt */
    ' ',  /* Space bar */
      0,  /* Caps lock */
      0,  0, 0, 0, 0, 0, 0, 0, 0, 0, /* F1-F10 */
      0,  /* Num lock */
      0,  /* Scroll lock */
      0,  /* Home */
      0,  /* Up arrow */
      0,  /* Page up */
    '-',
      0,  /* Left arrow */
      0,
      0,  /* Right arrow */
    '+',
      0,  /* End */
      0,  /* Down arrow */
      0,  /* Page down */
      0,  /* Insert */
      0,  /* Delete */
      0, 0, 0,
      0,  /* F11 */
      0,  /* F12 */
      0,
};

static char              kb_buffer[KEYBOARD_BUFFER_SIZE];
static volatile uint8_t  kb_read_pos  = 0;
static volatile uint8_t  kb_write_pos = 0;

void keyboard_initialize(void) {
    kb_read_pos  = 0;
    kb_write_pos = 0;
}

void keyboard_handle_scancode(uint8_t scancode) {
    if (scancode & 0x80)
        return; /* key-release event */

    if (scancode >= sizeof(keyboard_map))
        return;

    char c = keyboard_map[scancode];
    if (c == 0)
        return;

    uint8_t next = (kb_write_pos + 1) % KEYBOARD_BUFFER_SIZE;
    if (next == kb_read_pos)
        return; /* buffer full — drop character */

    kb_buffer[kb_write_pos] = c;
    kb_write_pos = next;
}

int keyboard_available(void) {
    return kb_read_pos != kb_write_pos;
}

char keyboard_getchar(void) {
    while (!keyboard_available()); /* spin until data arrives */
    char c = kb_buffer[kb_read_pos];
    kb_read_pos = (kb_read_pos + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}
