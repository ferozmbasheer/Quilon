#include <stdint.h>
#include <stdio.h>

#include <kernel/tty.h>
#include <kernel/interrupts.h>
#include <kernel/gdt.h>
#include <kernel/multiboot.h>
#include <kernel/pmm.h>
#include <kernel/paging.h>
#include <kernel/kmalloc.h>
#include <kernel/serial.h>
#include <kernel/keyboard.h>
#include <kernel/shell.h>
#include <kernel/pit.h>
#include <kernel/scheduler.h>
#include <kernel/usermode.h>
#include <kernel/syscall.h>
#include <kernel/ata.h>
#include <kernel/vfs.h>
#include <kernel/fat16.h>

extern uint32_t multiboot_info_ptr;

/* Thin wrapper so fat16_ctx_t can call ata_read_sectors via a function pointer.
 * ctx is the drive index (ATA_MASTER / ATA_SLAVE) cast to void*. */
static int ata_sector_read(void *ctx, uint32_t lba, void *buf)
{
	return ata_read_sectors((int)(uintptr_t)ctx, lba, 1, buf);
}

static fat16_ctx_t fs_ctx;

void kernel_main(void) {
	serial_initialize();
	gdt_initialize();
	idt_initialize();
	terminal_initialize();
	keyboard_initialize();
	scheduler_initialize();
	pit_initialize(100); /* 100 Hz — 10 ms tick */
	syscall_initialize();

	multiboot_info_t *mbi = (multiboot_info_t *)multiboot_info_ptr;
	pmm_initialize(mbi);
	paging_initialize();
	kmalloc_initialize();

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
	printf("PMM: %d KB free\r\n", (int)(pmm_free_page_count() * (PAGE_SIZE / 1024)));

	/* Sample allocations — smoke-test the PMM. */
	void *page_a = pmm_alloc_page();
	void *page_b = pmm_alloc_page();
	printf("alloc: 0x%x  0x%x\r\n", (unsigned int)page_a, (unsigned int)page_b);

	pmm_free_page(page_a);
	printf("after free: %d KB free\r\n", (int)(pmm_free_page_count() * (PAGE_SIZE / 1024)));

	/* ── Heap allocator smoke-tests ─────────────────────────────────────────
	 * Verify kmalloc/kfree basics: allocation, independence, and free+reuse. */

	/* 1. Two independent allocations must return different, non-NULL pointers. */
	uint32_t *a = (uint32_t *)kmalloc(sizeof(uint32_t));
	uint32_t *b = (uint32_t *)kmalloc(sizeof(uint32_t));
	printf("heap: a=0x%x  b=0x%x\r\n", (unsigned)a, (unsigned)b);

	kmalloc_dump();

	/* 2. Writes to one allocation must not corrupt the other. */
	*a = 0xCAFEBABE;
	*b = 0xDEADBEEF;
	printf("heap: *a=0x%x (expect 0xcafebabe)  *b=0x%x (expect 0xdeadbeef)\r\n",
	       (unsigned)*a, (unsigned)*b);

	kmalloc_dump();

	/* 3. After freeing `a`, a fresh allocation of the same size should
	 *    reuse the same address (first-fit, no other freed blocks ahead). */
	
	printf("Freeing a\r\n");

	kfree(a);

	kmalloc_dump();

	uint32_t *c = (uint32_t *)kmalloc(sizeof(uint32_t));
	printf("heap: after free+realloc c=0x%x (expect 0x%x)\r\n",
	       (unsigned)c, (unsigned)a);

	kmalloc_dump();

	/* 4. A larger allocation to exercise splitting. */
	char *buf = (char *)kmalloc(256);
	printf("heap: 256-byte buf=0x%x\r\n", (unsigned)buf);
	kfree(buf);
	kfree(b);
	kfree(c);

	/* 5. Dump the heap — should show a single large free block after all
	 *    the frees and coalescing above.                                   */
	kmalloc_dump();

	/* ── Filesystem initialisation ─────────────────────────────────────────
	 * Probe the primary ATA bus for a disk and attempt to mount it as FAT16.
	 * If no drive is found, or the first sector is not a valid FAT16 volume,
	 * the shell still works — ls/cat will report "No filesystem mounted."  */

	int drives_found = ata_initialize();
	printf("ata: %d drive(s) detected\r\n", drives_found);

	/* Try master first, then slave — mount whichever has a valid FAT16 volume. */
	int fs_drive = -1;
	if (ata_drive_present(ATA_MASTER))      fs_drive = ATA_MASTER;
	else if (ata_drive_present(ATA_SLAVE))  fs_drive = ATA_SLAVE;

	if (fs_drive >= 0) {
		const char *drive_name = (fs_drive == ATA_MASTER) ? "master" : "slave";
		fs_ctx.sector_read = ata_sector_read;
		fs_ctx.ctx         = (void *)(uintptr_t)fs_drive;

		if (fat16_mount(&fs_ctx) == 0) {
			vfs_mount(&fat16_vfs_ops, &fs_ctx);
			printf("fs: FAT16 mounted on primary %s\r\n", drive_name);
		} else {
			printf("fs: primary %s is not a FAT16 volume\r\n", drive_name);
		}
	} else {
		printf("fs: no disk detected — filesystem unavailable\r\n");
	}

	printf("   ___        _ _ \r\n");
	printf("  / _ \\ _   _(_) | ___  _ __  \r\n");
	printf(" | | | | | | | | |/ _ \\| '_ \\ \r\n");
	printf(" | |_| | |_| | | | (_) | | | |\r\n");
	printf("  \\__\\_\\__,__|_|_|\\___/|_| |_|\r\n\n");

	shell_run();
}
