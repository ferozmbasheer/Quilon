/* ── AP startup trampoline ─────────────────────────────────────────────────
 *
 * Copied to physical 0x8000 by smp_boot_aps() before any SIPI is sent.
 * An AP receiving a SIPI with vector=8 wakes in 16-bit real mode with:
 *   CS = 0x0800 (segment base 0x8000), IP = 0x0000
 *
 * The trampoline:
 *   0x0000  16-bit real-mode prologue (cli, GDT load, PE enable, far jump)
 *   0x0040  32-bit protected-mode entry (segment setup, paging on, jmp to C)
 *   0x0080  Minimal GDT descriptor (GDTR, null/code/data entries)
 *
 * BSP-to-AP communication area (also in the trampoline page):
 *   0x8F00  uint32_t  kernel CR3 (physical address of kernel page directory)
 *   0x8F04  uint32_t  ap_entry_c() virtual address
 *   0x8F08  uint32_t  per-AP kernel stack top (virtual address)
 *
 * All three slots are filled by smp_boot_aps() before each SIPI is sent.
 * Because APs are started one at a time and the BSP waits for each to check
 * in before continuing, it is safe to reuse these three slots for every AP.
 */

.section .smp_trampoline, "ax"

/* ── 16-bit real-mode entry ───────────────────────────────────────────────── */
.code16
.global smp_trampoline_start
smp_trampoline_start:                   /* physical 0x8000 after copy        */
    cli
    cld

    /* Zero DS/ES/SS so that all data references are flat from 0x0000.      */
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* Load the minimal GDT descriptor sitting at physical 0x8080.
     * We put the address in EAX first to get reliable 32-bit addressing.   */
    movl    $0x8080, %eax
    lgdtl   (%eax)              /* operand-size prefix → 32-bit GDT base    */

    /* Enable Protected Mode: set CR0 bit 0 (PE).                           */
    movl    %cr0, %eax
    orl     $0x1, %eax
    movl    %eax, %cr0

    /* Far jump flushes the instruction pipeline and reloads CS=0x08.
     * Target is the 32-bit code at physical 0x8040 (trampoline offset 0x40).
     * Encoded manually: 0x66 (operand-size override) + 0xEA (far jmp)
     * followed by 32-bit offset 0x00008040 and 16-bit selector 0x0008.    */
    .byte   0x66, 0xEA
    .long   0x00008040
    .word   0x0008

/* ── 32-bit protected-mode entry at trampoline offset 0x40 ──────────────── */
.org 0x40
.code32
smp_trampoline_32:
    /* Load the flat kernel data selector (0x10) into all data segments.    */
    movl    $0x10, %eax
    movw    %ax,   %ds
    movw    %ax,   %es
    movw    %ax,   %fs
    movw    %ax,   %gs
    movw    %ax,   %ss

    /* Load kernel CR3 written by BSP at 0x8F00 and switch page tables.     */
    movl    0x8F00, %eax
    movl    %eax,  %cr3

    /* Enable Paging: set CR0 bit 31 (PG).                                  */
    movl    %cr0, %eax
    orl     $0x80000000, %eax
    movl    %eax, %cr0

    /* Load per-AP kernel stack top from 0x8F08 (a kernel-high VA).         */
    movl    0x8F08, %esp

    /* Jump to ap_entry_c() whose virtual address BSP stored at 0x8F04.
     * After paging is on, kernel-high VAs (0xC0xxxxxx) are reachable.      */
    movl    0x8F04, %eax
    jmp     *%eax

1:  hlt
    jmp     1b

/* ── Minimal bootstrap GDT at trampoline offset 0x80 ───────────────────── */
.org 0x80
trampoline_gdtr:
    .word   0x17            /* GDT limit = 3 entries × 8 bytes − 1 = 23    */
    .long   0x8088          /* GDT base  = physical 0x8088 (offset 0x88)   */

.org 0x88
    /* [0] Null descriptor */
    .quad   0x0000000000000000
    /* [1] Kernel code: base=0, limit=4G, 32-bit, DPL=0, exec+read        */
    .quad   0x00CF9A000000FFFF
    /* [2] Kernel data: base=0, limit=4G, 32-bit, DPL=0, read+write       */
    .quad   0x00CF92000000FFFF

.global smp_trampoline_end
smp_trampoline_end:
    .byte   0
