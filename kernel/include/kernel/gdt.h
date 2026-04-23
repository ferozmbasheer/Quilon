#ifndef _KERNEL_GDT_H
#define _KERNEL_GDT_H

#include <stddef.h>
#include <stdint.h>

#define GDTBASE 0x00000800
#define GDTSIZE 0xFF

/* ── GDT Access Byte bit definitions ────────────────────────────────────── */
/* Each GDT descriptor's access byte encodes the segment type and privilege. */
#define GDT_ACCESS_PRESENT    (1 << 7)  /* Segment is present in memory      */
#define GDT_ACCESS_RING0      (0 << 5)  /* Descriptor Privilege Level: ring 0 */
#define GDT_ACCESS_RING3      (3 << 5)  /* Descriptor Privilege Level: ring 3 */
#define GDT_ACCESS_CODE_SEG   (1 << 4)  /* S bit: 1 = code/data segment      */
#define GDT_ACCESS_EXECUTABLE (1 << 3)  /* Executable segment (code)         */
#define GDT_ACCESS_DC         (1 << 2)  /* Direction/Conforming bit          */
#define GDT_ACCESS_READWRITE  (1 << 1)  /* Readable (code) / Writable (data) */
#define GDT_ACCESS_ACCESSED   (1 << 0)  /* Accessed bit (set by CPU)         */
#define GDT_ACCESS_TSS        (0x09)    /* System segment: 32-bit TSS type   */

/* ── GDT Flags Nibble bit definitions ('other' parameter) ───────────────── */
#define GDT_FLAG_AVL          (1 << 0)  /* Available for OS use              */
#define GDT_FLAG_64BIT        (1 << 1)  /* L: 64-bit mode (IA-32e)           */
#define GDT_FLAG_32BIT        (1 << 2)  /* DB: 32-bit protected mode         */
#define GDT_FLAG_GRANULARITY  (1 << 3)  /* G: 4 KiB page granularity         */

/* Standard flags for 32-bit segments: 4 KiB pages + 32-bit mode + AVL */
#define GDT_FLAGS_32BIT_PAGE  (GDT_FLAG_GRANULARITY | GDT_FLAG_32BIT | GDT_FLAG_AVL)

/* ── Pre-built access bytes for each segment type ───────────────────────── */
/* Kernel code:  present | ring 0 | S=1 | executable | readable | accessed  */
#define GDT_KERNEL_CODE  (GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | \
                          GDT_ACCESS_CODE_SEG | GDT_ACCESS_EXECUTABLE | \
                          GDT_ACCESS_READWRITE | GDT_ACCESS_ACCESSED)

/* Kernel data:  present | ring 0 | S=1 | writable | accessed               */
#define GDT_KERNEL_DATA  (GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | \
                          GDT_ACCESS_CODE_SEG | GDT_ACCESS_READWRITE | \
                          GDT_ACCESS_ACCESSED)

/* Kernel stack: present | ring 0 | S=1 | expand-down | writable | accessed */
#define GDT_KERNEL_STACK (GDT_ACCESS_PRESENT | GDT_ACCESS_RING0 | \
                          GDT_ACCESS_CODE_SEG | GDT_ACCESS_DC | \
                          GDT_ACCESS_READWRITE | GDT_ACCESS_ACCESSED)

/* User code:    present | ring 3 | S=1 | executable | readable | accessed  */
#define GDT_USER_CODE    (GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | \
                          GDT_ACCESS_CODE_SEG | GDT_ACCESS_EXECUTABLE | \
                          GDT_ACCESS_READWRITE | GDT_ACCESS_ACCESSED)

/* User data:    present | ring 3 | S=1 | writable | accessed               */
#define GDT_USER_DATA    (GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | \
                          GDT_ACCESS_CODE_SEG | GDT_ACCESS_READWRITE | \
                          GDT_ACCESS_ACCESSED)

/* User stack:   present | ring 3 | S=1 | expand-down | writable | accessed */
#define GDT_USER_STACK   (GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | \
                          GDT_ACCESS_CODE_SEG | GDT_ACCESS_DC | \
                          GDT_ACCESS_READWRITE | GDT_ACCESS_ACCESSED)

/* TSS:          present | ring 3 | S=0 | type=1001 (available 32-bit TSS)  */
#define GDT_TSS          (GDT_ACCESS_PRESENT | GDT_ACCESS_RING3 | GDT_ACCESS_TSS)

struct gdtdesc {
    uint16_t lim0_15;
    uint16_t base0_15;
    uint8_t base16_23;
    uint8_t acces;
    uint8_t lim16_19:4;
    uint8_t other:4;
    uint8_t base24_31;
} __attribute__ ((packed));

struct gdtr {
	uint16_t limit;
	uint32_t base;
} __attribute__ ((packed));

struct tss {
	uint16_t previous_task, __previous_task_unused;
	uint32_t esp0;
	uint16_t ss0, __ss0_unused;
	uint32_t esp1;
	uint16_t ss1, __ss1_unused;
	uint32_t esp2;
	uint16_t ss2, __ss2_unused;
	uint32_t cr3;
	uint32_t eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
	uint16_t es, __es_unused;
	uint16_t cs, __cs_unused;
	uint16_t ss, __ss_unused;
	uint16_t ds, __ds_unused;
	uint16_t fs, __fs_unused;
	uint16_t gs, __gs_unused;
	uint16_t ldt_selector, __ldt_sel_unused;
	uint16_t debug_flag, io_map;
} __attribute__ ((packed));

void gdt_initialize(void);

/*
 * gdt_set_kernel_stack — set the TSS esp0 field to esp0.
 *
 * Must be called on every context switch so that the CPU knows which
 * kernel stack to use when a ring-3 interrupt fires for the new process.
 */
void gdt_set_kernel_stack(uint32_t esp0);

#endif
