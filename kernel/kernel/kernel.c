#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/interrupts.h>
#include <kernel/gdt.h>
#include <kernel/multiboot.h>
#include <kernel/pmm.h>

extern uint32_t multiboot_info_ptr;

void kernel_main(void) {
	gdt_initialize();
	idt_initialize();
	terminal_initialize();

	multiboot_info_t *mbi = (multiboot_info_t *)multiboot_info_ptr;
	pmm_initialize(mbi);

	printf("Quilon OS v0.0.1\r\n");
	printf("PMM: %d KB free\r\n", (int)(pmm_free_page_count() * (PAGE_SIZE / 1024)));

	/* Sample allocations — smoke-test the PMM. */
	void *page_a = pmm_alloc_page();
	void *page_b = pmm_alloc_page();
	printf("alloc: 0x%x  0x%x\r\n", (unsigned int)page_a, (unsigned int)page_b);

	pmm_free_page(page_a);
	printf("after free: %d KB free\r\n", (int)(pmm_free_page_count() * (PAGE_SIZE / 1024)));
}
