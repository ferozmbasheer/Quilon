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
#include <kernel/elf.h>
#include <kernel/process.h>
#include <kernel/signal.h>
#include <kernel/initrd.h>
#include <kernel/pci.h>
#include <kernel/rtl8139.h>
#include <kernel/net.h>
#include <kernel/vbe.h>
#include <kernel/psf.h>
#include <kernel/apic.h>
#include <kernel/smp.h>

extern uint32_t multiboot_info_ptr;

/* Thin wrapper so fat16_ctx_t can call ata_read_sectors via a function pointer.
 * ctx is the drive index (ATA_MASTER / ATA_SLAVE) cast to void*. */
static int ata_sector_read(void *ctx, uint32_t lba, void *buf)
{
	return ata_read_sectors((int)(uintptr_t)ctx, lba, 1, buf);
}

static int ata_sector_write(void *ctx, uint32_t lba, const void *buf)
{
	return ata_write_sectors((int)(uintptr_t)ctx, lba, 1, buf);
}

static fat16_ctx_t fs_ctx;

void kernel_main(void) {
	serial_initialize();
	gdt_initialize();
	idt_initialize();
	terminal_initialize();
	keyboard_initialize();
	scheduler_initialize();
	process_init();
	pit_initialize(100); /* 100 Hz  - 10 ms tick */
	syscall_initialize();

	multiboot_info_t *mbi = (multiboot_info_t *)multiboot_info_ptr;
	pmm_initialize(mbi);
	paging_initialize();
	kmalloc_initialize();

	/* ── Paging smoke-tests ─────────────────────────────────────────────
	 * If any of these printf calls appear, the MMU is on and the kernel
	 * is still executing  - the identity mapping is working.             */

	/* 1. Read CR0 back; bit 31 (0x80000000) must be set. */
	uint32_t cr0;
	asm volatile("mov %%cr0, %%eax" : "=a"(cr0));
	printf("paging: CR0=0x%x  PG bit=%d\r\n",
	       (unsigned int)cr0, (int)((cr0 >> 31) & 1));

	/* 2. Read back the VGA buffer address (0xB8000) through the MMU.
	 * virtual 0xB8000 == physical 0xB8000 (identity-mapped in first 4MiB).
	 * The printf above already wrote 'p' (0x70) to cell (0,0), so reading
	 * vga[0] back should return 0x70  - proving the MMU round-trip works.  */
	uint8_t *vga = (uint8_t *)0xB8000;
	printf("paging: VGA[0]=0x%x (expected 0x70='p' written by prior line)\r\n",
	       (unsigned int)vga[0]);
	printf("PMM: %d KB free\r\n", (int)(pmm_free_page_count() * (PAGE_SIZE / 1024)));

	/* Sample allocations  - smoke-test the PMM. */
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


	/* 3. After freeing `a`, a fresh allocation of the same size should
	 *    reuse the same address (first-fit, no other freed blocks ahead). */
	kfree(a);


	uint32_t *c = (uint32_t *)kmalloc(sizeof(uint32_t));
	printf("heap: after free+realloc c=0x%x (expect 0x%x)\r\n",
	       (unsigned)c, (unsigned)a);



	/* 4. A larger allocation to exercise splitting. */
	char *buf = (char *)kmalloc(256);
	printf("heap: 256-byte buf=0x%x\r\n", (unsigned)buf);
	kfree(buf);
	kfree(b);
	kfree(c);

	/* 5. Dump the heap  - should show a single large free block after all
	 *    the frees and coalescing above.                                   */
	kmalloc_dump();

	/* ── Section 8.3: initrd - RAM-Based Initial Filesystem ────────────────
	 *
	 * An initrd is a small filesystem embedded in RAM, available at boot
	 * before any disk drivers are initialised.  GRUB passes it as a
	 * Multiboot module (add  --module /path/to/initrd.img  in grub.cfg).
	 *
	 * VFS is single-mount: if a FAT16 disk is found below, it replaces
	 * the initrd.  If there is no disk the initrd remains the active FS.
	 *
	 * Interactive demo:  quilon> initrd
	 * ─────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 8.3: initrd RAM filesystem ===\r\n");
	{
		static initrd_ctx_t grub_initrd_ctx;

		/* Check for a GRUB Multiboot module and mount it as the initrd. */
		if ((mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0) {
			multiboot_module_t *mod =
			    (multiboot_module_t *)(uintptr_t)mbi->mods_addr;
			void    *mod_addr = (void *)(uintptr_t)mod->mod_start;
			uint32_t mod_size = mod->mod_end - mod->mod_start;
			printf("initrd: GRUB module at 0x%x  (%d bytes)\r\n",
			       (unsigned)mod->mod_start, (int)mod_size);
			if (initrd_mount(&grub_initrd_ctx, mod_addr, mod_size) == 0) {
				vfs_mount(&initrd_vfs_ops, &grub_initrd_ctx);
				printf("initrd: mounted %d file(s) from Multiboot module\r\n",
				       (int)grub_initrd_ctx.file_count);

				/* Auto-load PSF2 bitmap font if present in the initrd. */
				for (uint32_t _fi = 0; _fi < grub_initrd_ctx.file_count; _fi++) {
					initrd_entry_t *fe = &grub_initrd_ctx.files[_fi];
					const char *fn = fe->name;
					if (fn[0]=='F' && fn[1]=='O' && fn[2]=='N' && fn[3]=='T' &&
					    fn[4]=='.' && fn[5]=='P' && fn[6]=='S' && fn[7]=='F' &&
					    fn[8]=='\0') {
						if (psf2_load(fe->data, fe->size) == 0) {
							const psf2_font_t *pf = psf2_get_font();
							printf("psf: loaded FONT.PSF: %dx%d  %d glyphs\r\n",
							       (int)pf->width, (int)pf->height,
							       (int)pf->glyph_count);
						} else {
							printf("psf: FONT.PSF parse failed\r\n");
						}
						break;
					}
				}
			} else {
				printf("initrd: module is not a valid initrd image\r\n");
			}
		} else {
			printf("initrd: no GRUB module  "
			       "(add --module in grub.cfg to load a real initrd)\r\n");
		}

		/* Build a synthetic 3-file image to demonstrate the driver.
		 * If no GRUB module was provided, also mount it so the kernel has
		 * a working filesystem before ATA initialisation completes.       */
		static uint8_t    demo_img[256];
		static initrd_ctx_t demo_ctx;
		uint32_t demo_size = initrd_build_demo(demo_img, sizeof(demo_img));

		if (demo_size > 0 &&
		    initrd_mount(&demo_ctx, demo_img, demo_size) == 0) {
			printf("initrd: synthetic demo: %d files\r\n",
			       (int)demo_ctx.file_count);

			/* List all entries. */
			vfs_dirent_t ent;
			for (uint32_t i = 0; i < demo_ctx.file_count; i++) {
				if (initrd_vfs_ops.readdir(&demo_ctx, "/", i, &ent) == 0)
					printf("  [%d] %s  (%d bytes)\r\n",
					       (int)i, ent.name,
					       (int)ent.size);
			}

			/* Read MOTD.TXT through the driver ops to prove data access. */
			vfs_node_t nd = {0};
			if (initrd_vfs_ops.open(&demo_ctx, "MOTD.TXT", &nd) == 0) {
				char buf[64];
				int nr = initrd_vfs_ops.read(&demo_ctx, &nd, 0,
				                             sizeof(buf) - 1,
				                             (uint8_t *)buf);
				if (nr > 0) {
					buf[nr] = '\0';
					printf("  MOTD.TXT: \"%s\"\r\n", buf);
				}
			}

			/* Fall back to synthetic mount only if no GRUB initrd loaded. */
			if (!vfs_mounted()) {
				vfs_mount(&initrd_vfs_ops, &demo_ctx);
				printf("initrd: synthetic image active as VFS fallback\r\n");
			}
		}
	}
	printf("=== Section 8.3 ready ===\r\n\r\n");

	/* ── Filesystem initialisation ─────────────────────────────────────────
	 * Probe the primary ATA bus for a disk and attempt to mount it as FAT16.
	 * If no drive is found, or the first sector is not a valid FAT16 volume,
	 * the shell still works  - ls/cat will report "No filesystem mounted."  */

	int drives_found = ata_initialize();
	printf("ata: %d drive(s) detected\r\n", drives_found);

	/* Try master first, then slave  - mount whichever has a valid FAT16 volume. */
	int fs_drive = -1;
	if (ata_drive_present(ATA_MASTER))      fs_drive = ATA_MASTER;
	else if (ata_drive_present(ATA_SLAVE))  fs_drive = ATA_SLAVE;

	if (fs_drive >= 0) {
		const char *drive_name = (fs_drive == ATA_MASTER) ? "master" : "slave";
		fs_ctx.sector_read  = ata_sector_read;
		fs_ctx.sector_write = ata_sector_write;
		fs_ctx.ctx          = (void *)(uintptr_t)fs_drive;

		if (fat16_mount(&fs_ctx) == 0) {
			vfs_mount(&fat16_vfs_ops, &fs_ctx);
			printf("fs: FAT16 mounted on primary %s\r\n", drive_name);
		} else {
			printf("fs: primary %s is not a FAT16 volume\r\n", drive_name);
		}
	} else {
		printf("fs: no disk detected  - filesystem unavailable\r\n");
	}

	/* ── ELF loader  - scan for executable files (section 4.12) ───────────────
	 * Walk the root directory and report any .ELF files found.
	 * The ELF loader itself is invoked interactively with the shell `exec`
	 * command:  quilon> exec HELLO.ELF
	 *
	 * To create a runnable ELF for Quilon, cross-compile a freestanding
	 * i386 program and copy it to the disk image (see BUILD.md).         */
	if (vfs_mounted()) {
		vfs_dirent_t elf_ent;
		uint32_t     elf_idx   = 0;
		int          elf_found = 0;

		while (vfs_readdir(elf_idx, &elf_ent) == 0) {
			/* Check for a ".ELF" extension (case-insensitive) */
			const char *nm = elf_ent.name;
			int len = 0;
			while (nm[len]) len++;

			if (len >= 4 &&
			    nm[len - 4] == '.' &&
			    (nm[len - 3] == 'E' || nm[len - 3] == 'e') &&
			    (nm[len - 2] == 'L' || nm[len - 2] == 'l') &&
			    (nm[len - 1] == 'F' || nm[len - 1] == 'f')) {
				printf("elf: found '%s' (%d bytes)  - "
				       "run with: exec %s\r\n",
				       elf_ent.name, (int)elf_ent.size,
				       elf_ent.name);
				elf_found = 1;
			}
			elf_idx++;
		}
		if (!elf_found)
			printf("elf: no .ELF files on disk "
			       "(see ROADMAP.md 4.12 and BUILD.md)\r\n");
	}

	/* ── Section 9.1: Higher-Half Kernel demo ─────────────────────────────────
	 *
	 * The kernel is now linked at virtual 0xC0100000 (higher half) but loaded
	 * by GRUB at physical 0x100000.  KERNEL_OFFSET = 0xC0000000 is the
	 * difference between the two addresses.
	 *
	 * Both PD[0] (identity map) and PD[768] (kernel-high) point to the same
	 * physical page table, so physical 0x001xxxxx is accessible both as
	 * virtual 0x001xxxxx (identity) and as 0xC01xxxxx (kernel symbol address).
	 * ──────────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 9.1: Higher-Half Kernel ===\r\n");
	{
		extern uint32_t kernel_start, kernel_end;
		uint32_t k_virt = (uint32_t)(uintptr_t)&kernel_start;
		uint32_t k_phys = k_virt - KERNEL_OFFSET;
		uint32_t k_size = (uint32_t)(uintptr_t)&kernel_end -
		                  (uint32_t)(uintptr_t)&kernel_start;

		printf("higher-half: KERNEL_OFFSET = 0x%x\r\n",
		       (unsigned)KERNEL_OFFSET);
		printf("higher-half: KERNEL_PD_IDX = %d  "
		       "(PD entry for 0xC0000000)\r\n",
		       (int)KERNEL_PD_IDX);
		printf("higher-half: kernel_start virt=0x%x  phys=0x%x\r\n",
		       (unsigned)k_virt, (unsigned)k_phys);
		printf("higher-half: kernel size = %d bytes\r\n", (int)k_size);
		printf("higher-half: kernel PD phys = 0x%x  "
		       "(loaded in CR3)\r\n",
		       (unsigned)paging_kernel_cr3());

		/* Demonstrate that the same physical page is reachable via both the
		 * identity map (low virtual == physical) and the kernel-high map.   */
		volatile uint32_t *via_identity = (volatile uint32_t *)(uintptr_t)k_phys;
		volatile uint32_t *via_high     = (volatile uint32_t *)(uintptr_t)k_virt;
		printf("higher-half: read via identity  0x%x -> 0x%x\r\n",
		       (unsigned)k_phys, (unsigned)*via_identity);
		printf("higher-half: read via high-map  0x%x -> 0x%x  (same value)\r\n",
		       (unsigned)k_virt, (unsigned)*via_high);
		printf("higher-half: alias match: %s\r\n",
		       (*via_identity == *via_high) ? "yes" : "no");
	}
	printf("=== Section 9.1 ready ===\r\n\r\n");

	/* ── Section 9.3: Copy-on-Write fork demo ─────────────────────────────────
	 *
	 * Shows that paging_fork_address_space() now uses CoW rather than eager
	 * page copies:
	 *   1. PMM reference counting: pmm_alloc_page sets refcount=1;
	 *      pmm_ref_page increments it; pmm_free_page only frees when it hits 0.
	 *   2. A CoW fork shares physical pages - free count stays the same after
	 *      the fork (the parent's writable pages are not duplicated).
	 *   3. PAGE_COW flag value and non-overlap with existing PTE flags.
	 *
	 * Interactive: quilon> fork   (ring-0 shell: reports -1, needs scheduler)
	 *              quilon> cow    (shows refcount lifecycle; SHELL.ELF shows live
	 *                             parent/child write isolation)
	 * ──────────────────────────────────────────────────────────────────────── */
	printf("=== Section 9.3: Copy-on-Write fork ===\r\n");
	{
		/* ── 1. PMM reference counting ───────────────────────────────── */
		uint32_t free_before = pmm_free_page_count();
		void *page_a = pmm_alloc_page();

		if (page_a) {
			printf("cow: alloc page_a @ 0x%x  refcount=%d  "
			       "(expect 1)\r\n",
			       (unsigned)(uintptr_t)page_a,
			       (int)pmm_page_refcount(page_a));

			pmm_ref_page(page_a);
			printf("cow: after pmm_ref_page    refcount=%d  "
			       "(expect 2)\r\n",
			       (int)pmm_page_refcount(page_a));

			pmm_free_page(page_a);   /* decrement to 1 */
			printf("cow: after first free      refcount=%d  "
			       "(expect 1, page NOT yet freed)\r\n",
			       (int)pmm_page_refcount(page_a));
			printf("cow: free pages after first free = %d  "
			       "(same as before alloc: %d)\r\n",
			       (int)pmm_free_page_count(), (int)free_before - 1);

			pmm_free_page(page_a);   /* decrement to 0 - now freed */
			printf("cow: after second free     free count = %d  "
			       "(back to %d: page returned)\r\n",
			       (int)pmm_free_page_count(), (int)free_before);
		}

		/* ── 2. PAGE_COW flag ──────────────────────────────────────── */
		printf("cow: PAGE_COW=0x%x  (bit 9, software-reserved)\r\n",
		       (unsigned)PAGE_COW);
		printf("cow: overlaps PRESENT?  %s  WRITABLE?  %s  USER?  %s\r\n",
		       (PAGE_COW & PAGE_PRESENT)  ? "YES (BUG)" : "no",
		       (PAGE_COW & PAGE_WRITABLE) ? "YES (BUG)" : "no",
		       (PAGE_COW & PAGE_USER)     ? "YES (BUG)" : "no");

		/* ── 3. CoW fork free-count invariant ─────────────────────── */
		uint32_t *pd_parent = paging_create_address_space();
		uint32_t *pd_child  = paging_create_address_space();
		if (pd_parent && pd_child) {
			/* Map a writable user page in the parent. */
			void *user_page = pmm_alloc_page();
			if (user_page) {
				paging_map_page_alloc_into(pd_parent, 0x00400000u,
				    (uint32_t)(uintptr_t)user_page,
				    PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

				uint32_t free_pre_fork  = pmm_free_page_count();
				paging_fork_address_space(pd_parent, pd_child);
				uint32_t free_post_fork = pmm_free_page_count();

				printf("cow: free pages before CoW fork: %d\r\n",
				       (int)free_pre_fork);
				printf("cow: free pages after  CoW fork: %d  "
				       "(only PT page allocated, not the data page)\r\n",
				       (int)free_post_fork);
				printf("cow: data page refcount after fork: %d  "
				       "(expect 2)\r\n",
				       (int)pmm_page_refcount(user_page));
			}
			pmm_free_page(pd_parent);
			pmm_free_page(pd_child);
		}

		printf("cow: use 'cow' at the shell prompt for interactive demo\r\n");
	}
	printf("=== Section 9.3 ready ===\r\n\r\n");

	/* ── Process isolation demo (section 5.1) ─────────────────────────────
	 * Show that two processes can have independent page directories at the
	 * same virtual address range without colliding.                       */
	{
		uint32_t *pd_a = paging_create_address_space();
		uint32_t *pd_b = paging_create_address_space();
		uint32_t *pd_k = paging_get_kernel_pd();

		printf("process: kernel PD @ 0x%x\r\n", (unsigned)pd_k);
		if (pd_a && pd_b) {
			printf("process: child A PD @ 0x%x  child B PD @ 0x%x\r\n",
			       (unsigned)pd_a, (unsigned)pd_b);
			printf("process: A != B: %s  (isolated address spaces)\r\n",
			       (pd_a != pd_b) ? "yes" : "no");
			printf("process: all share kernel entry[0]=0x%x  "
			       "(same page table)\r\n", pd_k[0]);
			pmm_free_page(pd_a);
			pmm_free_page(pd_b);
		}
		printf("process: table initialised  - %d slots, "
		       "use 'exec' to run in isolation\r\n", PROCESS_MAX);
	}

	/* ── Section 6: System Call Expansion demo ───────────────────────────────
	 *
	 * Prints the new syscall numbers (fork, sbrk, sigreturn) and the signal
	 * constants that the rest of the kernel now supports.
	 *
	 * Interactive demos:
	 *   quilon> sbrk    - ring-3 SYS_SBRK: allocate one page, write sentinel
	 *   quilon> fork    - ring-3 SYS_FORK: shows -1 from exec_setjmp path
	 *                    (proper fork needs a scheduled ring-3 process)
	 *
	 * Signal smoke-test: verify signal_send/dispatch with a synthetic process.
	 */
	{
		printf("\r\n=== Section 6: Syscall Expansion ===\r\n");

		/* New syscall numbers */
		printf("syscalls: SYS_FORK=%d  SYS_SBRK=%d  SYS_SIGRETURN=%d\r\n",
		       SYS_FORK, SYS_SBRK, SYS_SIGRETURN);

		/* Signal constants */
		printf("signals:  NSIG=%d  SIGKILL=%d  SIGSEGV=%d  SIGCHLD=%d\r\n",
		       NSIG, SIGKILL, SIGSEGV, SIGCHLD);

		/* Signal infrastructure smoke-test:
		 * Create a test process, send it SIGKILL via signal_send(), then
		 * verify the bit is set.  We do NOT call signal_dispatch() here
		 * because that would zombie the test process and call scheduler_yield()
		 * before a scheduler loop is running.                               */
		process_init();

		process_t *sig_test = process_create("sig-test", 0, 0);
		if (sig_test) {
			printf("signal test: created pid %d '%s'\r\n",
			       (int)sig_test->pid, sig_test->name);

			/* Initially no signals pending. */
			printf("signal test: pending before send = 0x%x (expect 0x0)\r\n",
			       (unsigned)sig_test->pending_signals);

			signal_send(sig_test, SIGKILL);
			printf("signal test: pending after SIGKILL = 0x%x (expect 0x%x)\r\n",
			       (unsigned)sig_test->pending_signals,
			       (unsigned)(1u << SIGKILL));

			signal_send(sig_test, SIGSEGV);
			printf("signal test: pending after SIGSEGV = 0x%x (expect 0x%x)\r\n",
			       (unsigned)sig_test->pending_signals,
			       (unsigned)((1u << SIGKILL) | (1u << SIGSEGV)));

			/* SIG_IGN: set handler, send, clear manually (no dispatch). */
			sig_test->signal_handlers[SIGCHLD] = SIG_IGN;
			signal_send(sig_test, SIGCHLD);
			printf("signal test: SIG_IGN set for SIGCHLD; "
			       "dispatch would ignore it\r\n");

			/* Clean up test process. */
			sig_test->state = PROC_UNUSED;
			printf("signal test: OK\r\n");
		}

		/* Re-initialise so the process table is clean for the shell. */
		process_init();

		printf("section 6 demo: use 'sbrk' or 'fork' at the shell prompt\r\n");
		printf("=== Section 6 ready ===\r\n\r\n");
	}

	/* ── Section 10.1: PCI Bus Enumeration ───────────────────────────────────
	 *
	 * Walk all 256 PCI buses × 32 slots.  Each non-empty slot is recorded in
	 * pci_devices[].  Multi-function devices (header type bit 7) have their
	 * extra functions probed individually.
	 *
	 * In QEMU the default i440FX machine exposes at minimum:
	 *   Bus 0 Slot 0  - Intel i440FX Host Bridge        (class 0x06)
	 *   Bus 0 Slot 1  - Intel PIIX3/PIIX4 ISA+IDE       (class 0x06)
	 *   Bus 0 Slot 2  - Bochs/QEMU VGA                  (class 0x03)
	 *
	 * Interactive demo:  quilon> pci
	 * ─────────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 10.1: PCI Bus Enumeration ===\r\n");
	{
		pci_enumerate();
		printf("pci: enumerated %d device(s)\r\n", pci_device_count);

		for (int i = 0; i < pci_device_count; i++) {
			const pci_device_t *d = &pci_devices[i];
			printf("pci: [%d] %d:%d.%d  vendor=0x%x  device=0x%x"
			       "  class=0x%x (%s)\r\n",
			       i,
			       (int)d->bus, (int)d->slot, (int)d->func,
			       (unsigned)d->vendor_id, (unsigned)d->device_id,
			       (unsigned)d->class_code,
			       pci_class_name(d->class_code));
		}

		/* Hint for section 10.2 - RTL8139 detection. */
		const pci_device_t *rtl =
		    pci_find_device(PCI_VENDOR_REALTEK, PCI_DEVICE_RTL8139);
		if (rtl)
			printf("pci: RTL8139 NIC @ %d:%d.%d  "
			       "(ready for section 10.2 driver)\r\n",
			       (int)rtl->bus, (int)rtl->slot, (int)rtl->func);
		else
			printf("pci: RTL8139 not found  "
			       "(add -device rtl8139 to qemu.sh for section 10.2)\r\n");

		printf("pci: use 'pci' at the shell prompt for interactive listing\r\n");
	}
	printf("=== Section 10.1 ready ===\r\n\r\n");

	/* ── Section 10.2: RTL8139 Network Card Driver ───────────────────────────
	 *
	 * Detects the RTL8139 via PCI, initialises it, prints the MAC address,
	 * and transmits one ARP broadcast frame to prove the TX path works.
	 *
	 * QEMU must be launched with:
	 *   -device rtl8139,netdev=net0 -netdev user,id=net0
	 * (qemu.sh already includes these flags after this section was added.)
	 *
	 * Interactive demo:  quilon> net       - show NIC status and MAC address
	 *                    quilon> netsend   - transmit a test ARP frame + poll RX
	 * ──────────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 10.2: RTL8139 Network Card Driver ===\r\n");
	{
		int nic_ok = rtl8139_init();
		if (nic_ok == 0) {
			uint8_t mac[RTL8139_MAC_LEN];
			rtl8139_get_mac(mac);
			printf("rtl8139: initialized OK\r\n");
			printf("rtl8139: MAC = %02x:%02x:%02x:%02x:%02x:%02x\r\n",
			       (unsigned)mac[0], (unsigned)mac[1],
			       (unsigned)mac[2], (unsigned)mac[3],
			       (unsigned)mac[4], (unsigned)mac[5]);

			/* Build and transmit a minimal ARP Who-has request broadcast.
			 *
			 * Frame layout (42 bytes):
			 *   [0–5]   Destination MAC  : FF:FF:FF:FF:FF:FF (broadcast)
			 *   [6–11]  Source MAC       : our hardware MAC
			 *   [12–13] EtherType        : 0x0806 (ARP)
			 *   [14–15] Hardware type    : 0x0001 (Ethernet)
			 *   [16–17] Protocol type    : 0x0800 (IPv4)
			 *   [18]    HW addr length   : 6
			 *   [19]    Proto addr length: 4
			 *   [20–21] ARP opcode       : 0x0001 (request)
			 *   [22–27] Sender MAC       : our MAC
			 *   [28–31] Sender IP        : 0.0.0.0 (unknown)
			 *   [32–37] Target MAC       : 00:00:00:00:00:00
			 *   [38–41] Target IP        : 10.0.2.2 (QEMU default gateway)
			 */
			static uint8_t arp_frame[42];
			int i;
			for (i = 0; i < 6; i++)  arp_frame[i]      = 0xFFu; /* dst: broadcast */
			for (i = 0; i < 6; i++)  arp_frame[6 + i]  = mac[i]; /* src: our MAC   */
			arp_frame[12] = 0x08; arp_frame[13] = 0x06;          /* EtherType: ARP */
			arp_frame[14] = 0x00; arp_frame[15] = 0x01;          /* HW: Ethernet   */
			arp_frame[16] = 0x08; arp_frame[17] = 0x00;          /* Proto: IPv4    */
			arp_frame[18] = 6;    arp_frame[19] = 4;              /* addr lengths   */
			arp_frame[20] = 0x00; arp_frame[21] = 0x01;          /* opcode: request*/
			for (i = 0; i < 6; i++)  arp_frame[22 + i] = mac[i]; /* sender MAC     */
			arp_frame[28] = 0; arp_frame[29] = 0;
			arp_frame[30] = 0; arp_frame[31] = 0;                 /* sender IP: 0   */
			for (i = 0; i < 6; i++)  arp_frame[32 + i] = 0;      /* target MAC: 0  */
			arp_frame[38] = 10; arp_frame[39] = 0;
			arp_frame[40] = 2;  arp_frame[41] = 2;                /* target: 10.0.2.2 */

			int tx_ok = rtl8139_send(arp_frame, (uint16_t)sizeof(arp_frame));
			printf("rtl8139: ARP broadcast %s\r\n",
			       tx_ok == 0 ? "transmitted OK" : "transmit FAILED");

			printf("rtl8139: use 'net' for status, 'netsend' for TX+RX demo\r\n");
		} else {
			printf("rtl8139: not found - add to qemu.sh:\r\n");
			printf("  -device rtl8139,netdev=net0 -netdev user,id=net0\r\n");
		}
	}
	printf("=== Section 10.2 ready ===\r\n\r\n");

	/* ── Section 10.3: Minimal TCP/IP Stack ──────────────────────────────────
	 *
	 * Initialises the TCP/IP stack on top of the RTL8139 driver.
	 * Provides: Ethernet II, ARP, IPv4, ICMP, UDP, TCP, and DHCP.
	 *
	 * Stack layers:
	 *   L2  Ethernet II  - eth_hdr_t   (14 bytes)
	 *   L3  ARP          - arp_pkt_t   (28 bytes)
	 *   L3  IPv4         - ipv4_hdr_t  (20 bytes)
	 *   L4  ICMP         - icmp_hdr_t  ( 8 bytes)
	 *   L4  UDP          - udp_hdr_t   ( 8 bytes)
	 *   L4  TCP          - tcp_hdr_t   (20 bytes)
	 *   App DHCP         - dhcp_msg_t  (300 bytes min)
	 *
	 * Interactive demos (after SHELL.ELF launches):
	 *   quilon> dhcp          - obtain IP via DHCP
	 *   quilon> ip            - show current IP address
	 *   quilon> ping 10.0.2.2 - ICMP echo to QEMU gateway
	 *   quilon> arp           - show ARP cache
	 * ──────────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 10.3: Minimal TCP/IP Stack ===\r\n");
	{
		printf("tcpip: initialising network stack...\r\n");
		int net_ok = net_init();
		if (net_ok == 0) {
			printf("tcpip: stack ready\r\n");
			printf("tcpip: protocols: Ethernet/ARP/IPv4/ICMP/UDP/TCP/DHCP\r\n");
			printf("tcpip: ARP cache size: %d entries\r\n",
			       (int)NET_ARP_CACHE_SIZE);
			printf("tcpip: UDP RX buffer: %d bytes  "
			       "TCP RX buffer: %d bytes\r\n",
			       (int)NET_UDP_MAX_PAYLOAD, (int)NET_TCP_RX_BUF);

			/* Print registered syscalls for this section. */
			printf("tcpip: SYS_NET_PING=%d  SYS_NET_DHCP=%d"
			       "  SYS_NET_GETIP=%d\r\n",
			       SYS_NET_PING, SYS_NET_DHCP, SYS_NET_GETIP);

			printf("tcpip: use 'dhcp' to acquire IP, "
			       "'ping <ip>' to test connectivity\r\n");
		} else {
			printf("tcpip: NIC not found - "
			       "add -device rtl8139 to qemu.sh\r\n");
		}
	}
	printf("=== Section 10.3 ready ===\r\n\r\n");

	/* ── Section 10.4: VGA Graphics Mode (VESA/VBE) ──────────────────────────
	 *
	 * Switches the display to a linear VESA framebuffer if GRUB negotiated a
	 * graphics mode (set gfxmode=800x600x32 + set gfxpayload=keep in grub.cfg).
	 * When available, all subsequent terminal_write() calls render to the
	 * pixel framebuffer instead of the VGA text buffer.
	 *
	 * Requires: paging and PMM initialised first (framebuffer pages are mapped
	 * via paging_map_page_alloc() in vbe_init()).
	 *
	 * Interactive demo:  quilon> vga   - draw gradient, color swatches, ASCII table
	 * ──────────────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 10.4: VGA Graphics Mode (VESA/VBE) ===\r\n");
	{
		if ((mbi->flags & MULTIBOOT_FLAG_FB) &&
		    mbi->framebuffer_type == MULTIBOOT_FB_TYPE_RGB) {
			vbe_info_t fb_info;
			fb_info.addr   = mbi->framebuffer_addr;
			fb_info.pitch  = mbi->framebuffer_pitch;
			fb_info.width  = mbi->framebuffer_width;
			fb_info.height = mbi->framebuffer_height;
			fb_info.bpp    = mbi->framebuffer_bpp;
			fb_info.type   = mbi->framebuffer_type;
			vbe_init(&fb_info);
			if (vbe_active()) {
				printf("vbe: framebuffer %dx%dx%d @ 0x%x  pitch=%d\r\n",
				       (int)fb_info.width, (int)fb_info.height,
				       (int)fb_info.bpp,
				       (unsigned)fb_info.addr,
				       (int)fb_info.pitch);
				printf("vbe: terminal %dx%d chars\r\n",
				       (int)vbe_term_cols(fb_info.width),
				       (int)vbe_term_rows(fb_info.height));
				vbe_demo();
			}
		} else {
			printf("vbe: no RGB framebuffer from GRUB "
			       "(add set gfxmode=800x600x32 to grub.cfg)\r\n");
		}
		printf("vbe: SYS_VBE_INFO=%d  - ring-3 framebuffer query\r\n",
		       SYS_VBE_INFO);
		printf("vbe: use 'vga' at the shell prompt to run the demo\r\n");
	}
	printf("=== Section 10.4 ready ===\r\n\r\n");

	/* ── Section 10.5: Symmetric Multiprocessing (SMP) ───────────────────────
	 *
	 * Initialises the Local APIC on the Bootstrap Processor (BSP), parses the
	 * Intel MP configuration table to discover Application Processors (APs),
	 * and boots each AP via an INIT+SIPI sequence.
	 *
	 * Each AP executes a 16-bit→32-bit real-mode trampoline (copied to physical
	 * 0x8000 before the first SIPI), enables paging with the kernel CR3, and
	 * calls ap_entry_c() where it reloads the GDT/IDT and enters an idle loop.
	 *
	 * A spinlock now protects pmm_alloc_page() / pmm_free_page() so that
	 * simultaneous memory allocation from multiple cores is safe.
	 *
	 * New files:
	 *   kernel/include/kernel/spinlock.h   — test-and-set spinlock
	 *   kernel/include/kernel/apic.h       — LAPIC register map + ICR helpers
	 *   kernel/include/kernel/smp.h        — cpu_info_t, MP table structs
	 *   kernel/arch/i386/apic.c            — LAPIC init, EOI, IPI send
	 *   kernel/arch/i386/smp.c             — MP table parse, AP boot, ap_entry_c
	 *   kernel/arch/i386/smp_trampoline.S  — 16-bit AP startup trampoline
	 *
	 * Try with QEMU option -smp 2 (already added to qemu.sh) to see two CPUs.
	 * ──────────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 10.5: Symmetric Multiprocessing (SMP) ===\r\n");

	apic_initialize();

	smp_initialize();
	printf("smp: found %d CPU(s) via MP table\r\n", (int)smp_cpu_count);
	for (uint32_t _ci = 0; _ci < smp_cpu_count; _ci++) {
		printf("  CPU %d: APIC-ID=%d %s\r\n",
		       (int)_ci,
		       (int)smp_cpus[_ci].apic_id,
		       smp_cpus[_ci].is_bsp ? "(BSP)" : "(AP)");
	}

	if (smp_cpu_count > 1) {
		smp_boot_aps();
		printf("smp: %d/%d CPU(s) online\r\n",
		       (int)smp_cpus_online, (int)smp_cpu_count);
	} else {
		printf("smp: single-CPU system (add -smp 2 to qemu.sh "
		       "for multi-core demo)\r\n");
	}
	printf("spinlock: PMM protected by spinlock_t (SMP-safe alloc/free)\r\n");
	printf("smp: use 'smp' at the shell prompt for CPU status\r\n");
	printf("=== Section 10.5 ready ===\r\n\r\n");

	printf("  ___        _ _ \r\n");
	printf(" / _ \\ _   _(_) | ___  _ __  \r\n");
	printf("| | | | | | | | |/ _ \\| '_ \\ \r\n");
	printf("| |_| | |_| | | | (_) | | | |\r\n");
	printf(" \\__\\_\\__,__|_|_|\\___/|_| |_|\r\n\n");

	/* ── Section 7: User Space ────────────────────────────────────────────────
	 *
	 * Attempt to load SHELL.ELF from the FAT16 disk and launch it as the
	 * first ring-3 process.  This replaces the kernel's ring-0 shell_run()
	 * call  - the kernel's role after boot becomes: initialise hardware,
	 * mount the disk, spawn SHELL.ELF, and yield to the scheduler.
	 *
	 * New in section 7:
	 *   7.1  user/libc/        - user-space C library (stdio, stdlib, string)
	 *                           with int $0x80 syscall stubs
	 *   7.2  user/shell/       - ring-3 shell built against user libc
	 *   7.3  user/hello_c/     - example C program using printf/malloc/exit
	 *   SYS_READDIR (12)       - new syscall: enumerate root directory entries
	 *   SYS_READ + FD_STDIN    - keyboard read path for ring-3 shell readline
	 *
	 * Fallback: if SHELL.ELF is not on disk (first build without section 7
	 * user binaries), the kernel falls back to the ring-0 shell_run() so the
	 * system remains usable during development.
	 * ──────────────────────────────────────────────────────────────────────── */
	/* ── Section 12.1: File Metadata & Directory Operations ─────────────────
	 *
	 * New syscalls: SYS_STAT(26), SYS_MKDIR(27), SYS_CHDIR(28),
	 *               SYS_GETCWD(29), SYS_LSEEK(30), SYS_RENAME(31)
	 *
	 * Exercises: stat a file, mkdir, ls to show the new dir, rename it,
	 *            and a lseek demo (open file, seek past first word, read rest).
	 *
	 * Interactive: stat/pwd/cd/mkdir/rename commands in both shells.
	 * ──────────────────────────────────────────────────────────────────────── */
	printf("\r\n=== Section 12.1: File Metadata & Directory Ops ===\r\n");
	printf("posix: SYS_STAT=%d  SYS_MKDIR=%d  SYS_CHDIR=%d\r\n",
	       SYS_STAT, SYS_MKDIR, SYS_CHDIR);
	printf("posix: SYS_GETCWD=%d  SYS_LSEEK=%d  SYS_RENAME=%d\r\n",
	       SYS_GETCWD, SYS_LSEEK, SYS_RENAME);

	if (vfs_mounted()) {
		/* 1. getcwd — should be "/" at boot */
		char cwd_buf[VFS_PATH_MAX];
		vfs_getcwd(cwd_buf, sizeof(cwd_buf));
		printf("posix: getcwd = \"%s\"\r\n", cwd_buf);

		/* 2. stat — probe the first directory entry for metadata */
		{
			vfs_dirent_t probe;
			if (vfs_readdir(0, &probe) == 0) {
				vfs_stat_t st;
				if (vfs_stat(probe.name, &st) == 0) {
					printf("posix: stat(\"%s\") = %s, %d bytes\r\n",
					       probe.name,
					       st.type == VFS_TYPE_DIR ? "dir" : "file",
					       (int)st.size);
				}
			}
		}

		/* 3. lseek demo — open first file with >= 4 bytes, read, seek, re-read */
		{
			vfs_dirent_t fent;
			uint32_t fi = 0;
			while (vfs_readdir(fi, &fent) == 0) {
				if (fent.type == VFS_TYPE_FILE && fent.size >= 4) break;
				fi++;
			}
			if (fent.type == VFS_TYPE_FILE && fent.size >= 4) {
				int lfd = vfs_open(fent.name);
				if (lfd >= 0) {
					char hdr[5];
					int hn = vfs_read(lfd, hdr, 4);
					hdr[hn > 0 ? hn : 0] = '\0';

					int new_pos = vfs_lseek(lfd, 2, VFS_SEEK_SET);
					printf("posix: lseek(\"%s\", 2, SET) -> pos=%d\r\n",
					       fent.name, new_pos);

					char tail[3];
					int tn = vfs_read(lfd, tail, 2);
					tail[tn > 0 ? tn : 0] = '\0';
					printf("posix: bytes[0..3]=%02x%02x%02x%02x  "
					       "re-read[2..3]=%02x%02x\r\n",
					       (unsigned)(uint8_t)hdr[0], (unsigned)(uint8_t)hdr[1],
					       (unsigned)(uint8_t)hdr[2], (unsigned)(uint8_t)hdr[3],
					       (unsigned)(uint8_t)tail[0], (unsigned)(uint8_t)tail[1]);
					vfs_close(lfd);
				}
			} else {
				printf("posix: lseek demo skipped (no file with >= 4 bytes)\r\n");
			}
		}
	} else {
		printf("posix: no filesystem mounted - demo skipped\r\n");
	}
	printf("=== Section 12.1 ready ===\r\n\r\n");

	printf("\r\n=== Section 7: User Space ===\r\n");
	printf("user space: SYS_READDIR=%d  (ring-3 ls)\r\n", SYS_READDIR);
	printf("user space: user/libc   - stdio/stdlib/string/syscall stubs\r\n");
	printf("user space: user/shell  - ring-3 C shell (SHELL.ELF)\r\n");
	printf("user space: user/hello_c  - C demo program (HELLOC.ELF)\r\n");

	if (vfs_mounted()) {
		/* Try to find and launch SHELL.ELF as the first ring-3 process. */
		uint32_t *shell_pd = paging_create_address_space();
		if (shell_pd) {
			vma_t shell_vmas[PROC_VMA_MAX];
			vma_init(shell_vmas, PROC_VMA_MAX);
			uint32_t shell_entry = elf_load_into("SHELL.ELF", shell_pd, shell_vmas);
			if (shell_entry != 0) {
				process_t *shell_proc =
				    process_create("shell", shell_entry,
				                   (uint32_t)(uintptr_t)shell_pd);
				if (shell_proc) {
					/* Copy VMAs from elf_load_into into the PCB. */
					for (int _v = 0; _v < PROC_VMA_MAX; _v++)
						shell_proc->vmas[_v] = shell_vmas[_v];

					printf("user space: launching SHELL.ELF "
					       "(pid=%d, entry=0x%x)\r\n",
					       (int)shell_proc->pid,
					       (unsigned)shell_entry);
					printf("=== Section 7 ready ===\r\n\r\n");

					/* Set as the running process and switch to its
					 * page directory before entering ring 3.          */
					current_process       = shell_proc;
					shell_proc->state     = PROC_RUNNING;
					paging_switch(shell_proc->cr3);

					/* process_launch() updates TSS, calls
					 * usermode_initialize(), then irets to ring 3.
					 * Never returns.                                   */
					process_launch();
					__builtin_unreachable();
				}
			}
			pmm_free_page(shell_pd);
		}
		printf("user space: SHELL.ELF not found  - falling back to "
		       "kernel ring-0 shell\r\n");
		printf("  (build user programs with: cd user && make)\r\n");
	} else {
		printf("user space: no filesystem  - cannot load SHELL.ELF\r\n");
	}
	printf("=== Section 7 fallback ===\r\n\r\n");

	/* ── Fallback: kernel ring-0 shell ──────────────────────────────────── */
	shell_run();
}
