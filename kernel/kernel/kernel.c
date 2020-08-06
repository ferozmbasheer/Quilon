#include <stdio.h>

#include <kernel/tty.h>

void kernel_main(void) {
	terminal_initialize();
	printf("Quilon OS v0.0.1\r\n");
	printf("\r\n");

	int i = 1;
	while(i) {
		printf("%c %s ", 'c' , "this");
	}
}
