#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/interrupts.h>
#include <kernel/gdt.h>

void kernel_main(void) {
	gdt_initialize();
	idt_initialize();

	terminal_initialize();

	printf("Quilon OS v0.0.1\r\n");
	printf("\r\n");

	printf("%c %s %d", 'c' , "this", 42);

	int i = 0;
	while(i<200)
	{
		printf("%d\r\n", i++);
	}
}