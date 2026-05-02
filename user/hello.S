/*
 * Quilon OS  - hello.S
 *
 * A minimal freestanding i386 user-mode program that communicates with the
 * Quilon kernel entirely through the int $0x80 system-call interface.
 *
 * How it runs
 * ───────────
 * 1. The kernel's `exec` shell command calls elf_load("HELLO.ELF").
 *    elf_load() opens the file from FAT16, reads it into a heap buffer,
 *    walks the PT_LOAD program headers, allocates fresh physical pages
 *    with pmm_alloc_page(), and maps them at virtual address 0x400000
 *    via paging_map_page_alloc().
 *
 * 2. elf_load() copies the code + data from the heap buffer into the
 *    newly mapped pages and returns e_entry = 0x400000 (_start).
 *
 * 3. The shell calls usermode_enter((void(*)(void))0x400000), which
 *    builds an iret frame on the kernel stack and switches the CPU to
 *    ring 3 (CPL=3).  Execution begins at _start.
 *
 * 4. _start calls SYS_WRITE (eax=1) several times, then SYS_EXIT (eax=3).
 *    The int $0x80 gate has DPL=3, so ring-3 code is allowed to fire it.
 *    Each invocation switches back to ring 0 via the TSS kernel stack,
 *    runs the syscall dispatcher, and returns to ring 3 via iret.
 *
 * System call convention (Quilon)
 * ───────────────────────────────
 *   eax = syscall number
 *   ebx = first argument
 *   ecx = second argument
 *   edx = third argument
 *   int $0x80
 *   return value in eax
 *
 * Syscalls used here:
 *   SYS_WRITE  = 1   write(fd=1, buf, len)  - fd 1 is FD_STDOUT
 *   SYS_EXIT   = 3   exit(code)
 *
 * Build
 * ─────
 *   See user/Makefile.  Cross-compiled with i686-elf-gcc and linked at
 *   virtual base 0x400000 (first 4 MiB is reserved for the kernel).
 */

.section .text
.global _start

_start:
    /* ── Print line 1 ─────────────────────────────────────────────────────── */
    movl  $1,     %eax        /* SYS_WRITE */
    movl  $1,     %ebx        /* FD_STDOUT */
    movl  $line1, %ecx        /* buf */
    movl  $line1_len, %edx    /* len */
    int   $0x80

    /* ── Print line 2 ─────────────────────────────────────────────────────── */
    movl  $1,     %eax
    movl  $1,     %ebx
    movl  $line2, %ecx
    movl  $line2_len, %edx
    int   $0x80

    /* ── Print line 3 ─────────────────────────────────────────────────────── */
    movl  $1,     %eax
    movl  $1,     %ebx
    movl  $line3, %ecx
    movl  $line3_len, %edx
    int   $0x80

    /* ── Print line 4 ─────────────────────────────────────────────────────── */
    movl  $1,     %eax
    movl  $1,     %ebx
    movl  $line4, %ecx
    movl  $line4_len, %edx
    int   $0x80

    /* ── Exit cleanly with code 0 ─────────────────────────────────────────── */
    movl  $3,     %eax        /* SYS_EXIT */
    movl  $0,     %ebx        /* exit code */
    int   $0x80

    /* SYS_EXIT never returns  - the kernel halts after printing an exit
     * message.  This spin is here only as a safety net.                   */
.Lhang:
    pause
    jmp   .Lhang

/* ── String data ─────────────────────────────────────────────────────────── */

line1:
    .ascii "\r\n"
    .ascii "+------------------------------------------+\r\n"
    .ascii "|   Hello from HELLO.ELF (Quilon ELF demo) |\r\n"
    .ascii "+------------------------------------------+\r\n"
line1_end:
.set line1_len, line1_end - line1

line2:
    .ascii "[elf] Binary compiled for i386, loaded from FAT16 disk.\r\n"
line2_end:
.set line2_len, line2_end - line2

line3:
    .ascii "[elf] Mapped to virtual 0x400000 by paging_map_page_alloc().\r\n"
line3_end:
.set line3_len, line3_end - line3

line4:
    .ascii "[elf] Running at ring 3 (CPL=3)  - calling SYS_EXIT now.\r\n"
line4_end:
.set line4_len, line4_end - line4
