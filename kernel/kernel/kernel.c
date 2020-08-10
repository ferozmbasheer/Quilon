#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/interrupts.h>

void kernel_main(void) {
	idt_initialize();
	terminal_initialize();
	printf("Quilon OS v0.0.1\r\n");
	printf("\r\n");


	//while(1);

	////int i = 1;
	//while(i) {
		printf("%c %s %d", 'c' , "this", 42);
	//}

	while(1);
}