#ifndef _KERNEL_MULTIBOOT_H
#define _KERNEL_MULTIBOOT_H

#include <stdint.h>

/* Bits in the multiboot_info_t.flags field */
#define MULTIBOOT_FLAG_MEM      (1 << 0)   /* mem_lower / mem_upper are valid  */
#define MULTIBOOT_FLAG_MMAP     (1 << 6)   /* mmap_length / mmap_addr are valid */

/* Values for multiboot_mmap_entry_t.type */
#define MULTIBOOT_MEMORY_AVAILABLE  1

/* Memory-map entry as written by GRUB.
 * 'size' does not include the size field itself; advance by size+4. */
typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;

/* The info struct that GRUB places in memory and whose address is in %ebx. */
typedef struct {
    uint32_t flags;
    uint32_t mem_lower;     /* KB of lower memory  (flags bit 0) */
    uint32_t mem_upper;     /* KB of upper memory  (flags bit 0) */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];       /* aout/elf symbol table info (16 bytes) */
    uint32_t mmap_length;   /* bytes in the mmap buffer (flags bit 6) */
    uint32_t mmap_addr;     /* physical address of the mmap buffer    */
} __attribute__((packed)) multiboot_info_t;

#endif /* _KERNEL_MULTIBOOT_H */
