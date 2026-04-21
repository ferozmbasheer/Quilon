#include <stdint.h>
#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/interrupts.h>
#include <kernel/gdt.h>
#include <kernel/multiboot.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>

extern uint32_t multiboot_info_ptr;

void kernel_main(void) {
	gdt_initialize();
	idt_initialize();
	terminal_initialize();

	multiboot_info_t *mbi = (multiboot_info_t *)multiboot_info_ptr;
	pmm_initialize(mbi);
	paging_initialize();

	/* ── Paging smoke-tests ─────────────────────────────────────────────
	 * If any of these printf calls appear, the MMU is on and the kernel
	 * is still executing — the identity mapping is working.             */

	/* 1. Read CR0 back; bit 31 (0x80000000) must be set. */
	uint32_t cr0;
	asm volatile("mov %%cr0, %0" : "=r"(cr0));
	printf("paging: CR0=0x%x  PG bit=%d\r\n",
	       (unsigned int)cr0, (int)((cr0 >> 31) & 1));

	/* 2. Read back the VGA buffer address (0xB8000) through the MMU.
	 * virtual 0xB8000 == physical 0xB8000 (identity-mapped in first 4MiB).
	 * The printf above already wrote 'p' (0x70) to cell (0,0), so reading
	 * vga[0] back should return 0x70 — proving the MMU round-trip works.  */
	uint8_t *vga = (uint8_t *)0xB8000;
	printf("paging: VGA[0]=0x%x (expected 0x70='p' written by prior line)\r\n",
	       (unsigned int)vga[0]);

	printf("Quilon OS v0.0.1\r\n");
	printf("PMM: %d KB free\r\n", (int)(pmm_free_page_count() * (PAGE_SIZE / 1024)));

	/* Sample allocations — smoke-test the PMM. */
	void *page_a = pmm_alloc_page();
	void *page_b = pmm_alloc_page();
	printf("alloc: 0x%x  0x%x\r\n", (unsigned int)page_a, (unsigned int)page_b);

	pmm_free_page(page_a);
	printf("after free: %d KB free\r\n", (int)(pmm_free_page_count() * (PAGE_SIZE / 1024)));
}
