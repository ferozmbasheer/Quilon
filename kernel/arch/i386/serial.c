#include <stddef.h>
#include <kernel/serial.h>

extern void outb(unsigned short port, unsigned char data);
extern char inb(unsigned short port);

#define COM1 0x3F8

void serial_initialize(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts                    */
    outb(COM1 + 3, 0x80); /* enable DLAB to set baud rate divisor  */
    outb(COM1 + 0, 0x03); /* divisor low byte:  3 -> 38400 baud     */
    outb(COM1 + 1, 0x00); /* divisor high byte                     */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, 1 stop bit         */
    outb(COM1 + 2, 0xC7); /* enable and clear FIFO, 14-byte thresh */
    outb(COM1 + 4, 0x0B); /* IRQs enabled, RTS/DSR set             */
}

void serial_putchar(char c) {
    /* Wait until the transmit-holding register is empty (bit 5 of LSR). */
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, c);
}

void serial_write(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++)
        serial_putchar(data[i]);
}
